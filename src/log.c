// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static edog_log_level_t g_min_level = EDOG_LOG_INFO;

static edog_log_entry_t g_ring[EDOG_LOG_RING_SIZE];
static uint64_t g_next_id = 1; /* id of the next entry; ring holds [g_next_id - N, g_next_id) */

static char g_sink_spec[160];
static pthread_t g_sink_thread;
static bool g_sink_started = false;
static atomic_bool g_sink_run = false;

const char *edog_log_level_name(edog_log_level_t level)
{
    switch (level) {
    case EDOG_LOG_DEBUG: return "DEBUG";
    case EDOG_LOG_INFO:  return "INFO";
    case EDOG_LOG_WARN:  return "WARN";
    case EDOG_LOG_ERROR: return "ERROR";
    }
    return "INFO";
}

void edog_log_set_level(edog_log_level_t level)
{
    g_min_level = level;
}

bool edog_log_parse_level(const char *name, edog_log_level_t *out)
{
    static const struct { const char *name; edog_log_level_t level; } map[] = {
        { "debug", EDOG_LOG_DEBUG }, { "info", EDOG_LOG_INFO },
        { "warn", EDOG_LOG_WARN },   { "warning", EDOG_LOG_WARN },
        { "error", EDOG_LOG_ERROR },
    };
    if (name == NULL || out == NULL)
        return false;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcasecmp(name, map[i].name) == 0) {
            *out = map[i].level;
            return true;
        }
    }
    return false;
}

void edog_logger_init(edog_logger_t *logger, const char *tag)
{
    if (logger == NULL)
        return;
    snprintf(logger->tag, sizeof(logger->tag), "%s", tag ? tag : "ETHERDOG");
}

uint64_t edog_log_next_id(void)
{
    pthread_mutex_lock(&g_mutex);
    uint64_t id = g_next_id;
    pthread_mutex_unlock(&g_mutex);
    return id;
}

static uint64_t oldest_id_locked(void)
{
    return g_next_id > EDOG_LOG_RING_SIZE ? g_next_id - EDOG_LOG_RING_SIZE : 1;
}

int edog_log_query(uint64_t min_id, edog_log_level_t min_level, edog_log_entry_t *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&g_mutex);
    uint64_t id = min_id < oldest_id_locked() ? oldest_id_locked() : min_id;
    for (; id < g_next_id && n < max; id++) {
        const edog_log_entry_t *e = &g_ring[id % EDOG_LOG_RING_SIZE];
        if (e->level >= min_level)
            out[n++] = *e;
    }
    pthread_mutex_unlock(&g_mutex);
    return n;
}

static void vlog(edog_logger_t *logger, edog_log_level_t level, const char *fmt, va_list ap)
{
    if (level < g_min_level)
        return;

    edog_log_entry_t e;
    memset(&e, 0, sizeof(e));
    vsnprintf(e.message, sizeof(e.message), fmt, ap);
    e.level = level;
    e.timestamp = (int64_t)time(NULL);
    snprintf(e.tag, sizeof(e.tag), "%s",
             (logger != NULL && logger->tag[0] != '\0') ? logger->tag : "ETHERDOG");

    char stamp[32] = "";
    struct tm tm_now;
    time_t now = (time_t)e.timestamp;
    if (localtime_r(&now, &tm_now) != NULL)
        strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_now);

    pthread_mutex_lock(&g_mutex);
    e.id = g_next_id++;
    g_ring[e.id % EDOG_LOG_RING_SIZE] = e;
    fprintf(stderr, "[%s] [%s] [%s] %s\n", stamp, edog_log_level_name(level), e.tag, e.message);
    fflush(stderr);
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);
}

#define DEFINE_LEVEL_FN(fn, level)                                  \
    void fn(edog_logger_t *logger, const char *fmt, ...)            \
    {                                                               \
        va_list ap;                                                 \
        va_start(ap, fmt);                                          \
        vlog(logger, level, fmt, ap);                               \
        va_end(ap);                                                 \
    }

DEFINE_LEVEL_FN(edog_log_debug, EDOG_LOG_DEBUG)
DEFINE_LEVEL_FN(edog_log_info, EDOG_LOG_INFO)
DEFINE_LEVEL_FN(edog_log_warn, EDOG_LOG_WARN)
DEFINE_LEVEL_FN(edog_log_error, EDOG_LOG_ERROR)

/* --- socket sink ------------------------------------------------------------------------ */

static size_t json_escape(char *out, size_t size, const char *in)
{
    size_t n = 0;
    for (; *in != '\0' && n + 7 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20) {
            n += (size_t)snprintf(out + n, size - n, "\\u%04x", c);
        } else {
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
    return n;
}

static int sink_connect(const char *spec)
{
    if (strncmp(spec, "unix:", 5) == 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        size_t len = strlen(spec + 5);
        if (len >= sizeof(addr.sun_path))
            return -1;
        memcpy(addr.sun_path, spec + 5, len + 1);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;
        if (fd >= 0)
            close(fd);
        return -1;
    }
    char host[64];
    const char *rest = spec + 4;
    const char *colon = strrchr(rest, ':');
    if (colon == NULL || (size_t)(colon - rest) >= sizeof(host))
        return -1;
    memcpy(host, rest, (size_t)(colon - rest));
    host[colon - rest] = '\0';
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)atoi(colon + 1));
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        return fd;
    if (fd >= 0)
        close(fd);
    return -1;
}

static bool send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, data, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

static void *sink_thread(void *arg)
{
    (void)arg;
    int fd = -1;
    uint64_t sent = 1;

    while (atomic_load(&g_sink_run)) {
        if (fd < 0) {
            fd = sink_connect(g_sink_spec);
            if (fd < 0) {
                struct timespec ts = { 1, 0 };
                nanosleep(&ts, NULL);
                continue;
            }
        }

        edog_log_entry_t e;
        bool have = false;
        pthread_mutex_lock(&g_mutex);
        if (sent < oldest_id_locked())
            sent = oldest_id_locked();
        if (sent >= g_next_id) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += 1;
            pthread_cond_timedwait(&g_cond, &g_mutex, &until);
        }
        if (sent < g_next_id) {
            e = g_ring[sent % EDOG_LOG_RING_SIZE];
            have = true;
        }
        pthread_mutex_unlock(&g_mutex);
        if (!have)
            continue;

        char msg[1100];
        char line[1400];
        json_escape(msg, sizeof(msg), e.message);
        bool plain = strcmp(e.tag, "ETHERDOG") == 0;
        int n = snprintf(line, sizeof(line),
                         "{\"timestamp\":\"%lld\",\"level\":\"%s\",\"source\":\"EtherDOG\","
                         "\"message\":\"[ETHERDOG]%s%s%s %s\"}\n",
                         (long long)e.timestamp, edog_log_level_name(e.level), plain ? "" : " [",
                         plain ? "" : e.tag, plain ? "" : "]", msg);
        if (n < 0)
            n = 0;
        if ((size_t)n >= sizeof(line))
            n = (int)sizeof(line) - 1;
        if (send_all(fd, line, (size_t)n)) {
            sent = e.id + 1;
        } else {
            close(fd);
            fd = -1; /* reconnect and resend this entry */
        }
    }
    if (fd >= 0)
        close(fd);
    return NULL;
}

int edog_log_start_sink(const char *spec)
{
    if (spec == NULL || (strncmp(spec, "unix:", 5) != 0 && strncmp(spec, "tcp:", 4) != 0) ||
        strlen(spec) >= sizeof(g_sink_spec))
        return -1;
    if (g_sink_started)
        return 0;
    snprintf(g_sink_spec, sizeof(g_sink_spec), "%s", spec);
    atomic_store(&g_sink_run, true);
    if (pthread_create(&g_sink_thread, NULL, sink_thread, NULL) != 0) {
        atomic_store(&g_sink_run, false);
        return -1;
    }
    g_sink_started = true;
    return 0;
}

void edog_log_stop_sink(void)
{
    if (!g_sink_started)
        return;
    atomic_store(&g_sink_run, false);
    pthread_mutex_lock(&g_mutex);
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);
    pthread_join(g_sink_thread, NULL);
    g_sink_started = false;
}
