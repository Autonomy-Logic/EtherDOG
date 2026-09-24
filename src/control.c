// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

#include "control.h"
#include "bus.h"
#include "log.h"
#include "version.h"
#include "cjson/cJSON.h"
#include "etherdog_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_CLIENTS 8
#define MAX_LINE (64 * 1024)
#define RESPONSE_SIZE (512 * 1024)

typedef struct {
    int fd;
    bool authenticated;
    size_t used;
    char buf[MAX_LINE];
} client_t;

static edog_logger_t g_log;
static client_t g_clients[MAX_CLIENTS];
static char g_response[RESPONSE_SIZE];
static char g_unix_path[108];

static bool constant_time_equals(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la != lb);
    for (size_t i = 0; i < la; i++)
        diff |= (unsigned char)(a[i] ^ b[i % (lb ? lb : 1)]);
    return diff == 0;
}

static int open_listener(const char *spec)
{
    if (strncmp(spec, "unix:", 5) == 0) {
        const char *path = spec + 5;
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (path[0] == '\0' || strlen(path) >= sizeof(addr.sun_path)) {
            edog_log_error(&g_log, "control socket path invalid: '%s'", path);
            return -1;
        }
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        unlink(addr.sun_path);
        mode_t old = umask(0077);
        int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
        umask(old);
        if (rc != 0 || listen(fd, MAX_CLIENTS) != 0) {
            edog_log_error(&g_log, "cannot listen on %s: %s", path, strerror(errno));
            close(fd);
            return -1;
        }
        chmod(addr.sun_path, 0600);
        snprintf(g_unix_path, sizeof(g_unix_path), "%s", addr.sun_path);
        edog_log_info(&g_log, "control socket: unix:%s", path);
        return fd;
    }

    if (strncmp(spec, "tcp:", 4) == 0) {
        char host[64];
        const char *rest = spec + 4;
        const char *colon = strrchr(rest, ':');
        if (colon == NULL || (size_t)(colon - rest) >= sizeof(host)) {
            edog_log_error(&g_log, "control endpoint invalid: '%s'", spec);
            return -1;
        }
        memcpy(host, rest, (size_t)(colon - rest));
        host[colon - rest] = '\0';
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        long port = strtol(colon + 1, NULL, 10);
        if (port <= 0 || port > 65535 || inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
            (ntohl(addr.sin_addr.s_addr) >> 24) != 127) {
            edog_log_error(&g_log, "control endpoint must be a loopback ip:port: '%s'", spec);
            return -1;
        }
        addr.sin_port = htons((uint16_t)port);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(fd, MAX_CLIENTS) != 0) {
            edog_log_error(&g_log, "cannot listen on %s: %s", spec, strerror(errno));
            close(fd);
            return -1;
        }
        edog_log_info(&g_log, "control socket: %s", spec);
        return fd;
    }

    edog_log_error(&g_log, "unsupported control endpoint '%s' (unix:<path> or tcp:<ip>:<port>)",
                   spec);
    return -1;
}

static void send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, data, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        data += n;
        len -= (size_t)n;
    }
}

static void reply(client_t *c, const char *json)
{
    send_all(c->fd, json, strlen(json));
    send_all(c->fd, "\n", 1);
}

static void drop_client(client_t *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->authenticated = false;
    c->used = 0;
}

static void hello_response(void)
{
    snprintf(g_response, sizeof(g_response),
             "{\"status\":\"success\",\"name\":\"EtherDOG\",\"version\":\"%s\",\"protocol\":%d}",
             EDOG_VERSION, EDOG_PROTOCOL_VERSION);
}

/* {"command":"logs","params":{"min_id":N,"level":"info","max":500}} */
static void logs_response(cJSON *root)
{
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    cJSON *min_id = params ? cJSON_GetObjectItemCaseSensitive(params, "min_id") : NULL;
    cJSON *level = params ? cJSON_GetObjectItemCaseSensitive(params, "level") : NULL;
    cJSON *max = params ? cJSON_GetObjectItemCaseSensitive(params, "max") : NULL;

    edog_log_level_t min_level = EDOG_LOG_DEBUG;
    if (cJSON_IsString(level) && !edog_log_parse_level(level->valuestring, &min_level)) {
        snprintf(g_response, sizeof(g_response), "{\"error\":\"unknown level\"}");
        return;
    }
    int limit = cJSON_IsNumber(max) ? max->valueint : 500;
    if (limit < 1)
        limit = 1;
    if (limit > EDOG_LOG_RING_SIZE)
        limit = EDOG_LOG_RING_SIZE;
    uint64_t from = cJSON_IsNumber(min_id) && min_id->valuedouble > 0 ? (uint64_t)min_id->valuedouble : 0;

    static edog_log_entry_t entries[EDOG_LOG_RING_SIZE];
    int n = edog_log_query(from, min_level, entries, limit);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "success");
    cJSON_AddNumberToObject(resp, "next_id", (double)(n > 0 ? entries[n - 1].id + 1 : edog_log_next_id()));
    cJSON *arr = cJSON_AddArrayToObject(resp, "entries");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "id", (double)entries[i].id);
        cJSON_AddNumberToObject(e, "timestamp", (double)entries[i].timestamp);
        cJSON_AddStringToObject(e, "level", edog_log_level_name(entries[i].level));
        cJSON_AddStringToObject(e, "source", entries[i].tag);
        cJSON_AddStringToObject(e, "message", entries[i].message);
        cJSON_AddItemToArray(arr, e);
    }
    char *text = cJSON_PrintUnformatted(resp);
    snprintf(g_response, sizeof(g_response), "%s", text ? text : "{\"error\":\"out of memory\"}");
    free(text);
    cJSON_Delete(resp);
}

/* Returns true when the server should exit. */
static bool handle_line(client_t *c, const char *line, const char *token)
{
    cJSON *root = cJSON_Parse(line);
    cJSON *cmd = root ? cJSON_GetObjectItemCaseSensitive(root, "command") : NULL;
    const char *name = (cmd && cJSON_IsString(cmd)) ? cmd->valuestring : NULL;
    bool need_auth = token != NULL && token[0] != '\0';

    if (name != NULL && strcmp(name, "hello") == 0) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        cJSON *tok = params ? cJSON_GetObjectItemCaseSensitive(params, "token") : NULL;
        if (need_auth && (tok == NULL || !cJSON_IsString(tok) ||
                          !constant_time_equals(tok->valuestring, token))) {
            reply(c, "{\"error\":\"authentication failed\"}");
            cJSON_Delete(root);
            drop_client(c);
            return false;
        }
        c->authenticated = true;
        hello_response();
        reply(c, g_response);
        cJSON_Delete(root);
        return false;
    }

    if (need_auth && !c->authenticated) {
        reply(c, "{\"error\":\"authenticate with 'hello' first\"}");
        cJSON_Delete(root);
        drop_client(c);
        return false;
    }

    if (name != NULL && strcmp(name, "logs") == 0) {
        logs_response(root);
        reply(c, g_response);
        cJSON_Delete(root);
        return false;
    }

    if (name != NULL && strcmp(name, "shutdown") == 0) {
        reply(c, "{\"status\":\"success\"}");
        cJSON_Delete(root);
        return true;
    }
    cJSON_Delete(root);

    ecat_bus_command(line, g_response, sizeof(g_response));
    reply(c, g_response);
    return false;
}

/* Returns true when the server should exit. */
static bool service_client(client_t *c, const char *token)
{
    ssize_t n = recv(c->fd, c->buf + c->used, sizeof(c->buf) - 1 - c->used, 0);
    if (n <= 0) {
        drop_client(c);
        return false;
    }
    c->used += (size_t)n;
    c->buf[c->used] = '\0';

    char *start = c->buf;
    char *nl;
    bool exit_now = false;
    while (!exit_now && c->fd >= 0 && (nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (nl > start && nl[-1] == '\r')
            nl[-1] = '\0';
        if (*start != '\0')
            exit_now = handle_line(c, start, token);
        start = nl + 1;
    }
    if (c->fd < 0)
        return exit_now;

    size_t rest = c->used - (size_t)(start - c->buf);
    memmove(c->buf, start, rest);
    c->used = rest;
    if (c->used >= sizeof(c->buf) - 1) {
        reply(c, "{\"error\":\"request line too long\"}");
        drop_client(c);
    }
    return exit_now;
}

int edog_control_run(const char *listen_spec, const char *token, volatile sig_atomic_t *stop)
{
    edog_logger_init(&g_log, "CONTROL");
    for (int i = 0; i < MAX_CLIENTS; i++)
        g_clients[i].fd = -1;

    int lfd = open_listener(listen_spec);
    if (lfd < 0)
        return -1;

    bool exit_now = false;
    while (!*stop && !exit_now) {
        struct pollfd pfd[MAX_CLIENTS + 1];
        int map[MAX_CLIENTS + 1];
        int n = 0;
        pfd[n].fd = lfd;
        pfd[n].events = POLLIN;
        map[n++] = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].fd >= 0) {
                pfd[n].fd = g_clients[i].fd;
                pfd[n].events = POLLIN;
                map[n++] = i;
            }
        }

        int rc = poll(pfd, (nfds_t)n, 200);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            edog_log_error(&g_log, "poll: %s", strerror(errno));
            break;
        }
        if (rc == 0)
            continue;

        if (pfd[0].revents & POLLIN) {
            int cfd = accept(lfd, NULL, NULL);
            if (cfd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (g_clients[i].fd < 0) {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0) {
                    const char *busy = "{\"error\":\"too many clients\"}\n";
                    send_all(cfd, busy, strlen(busy));
                    close(cfd);
                } else {
                    g_clients[slot].fd = cfd;
                    g_clients[slot].authenticated = false;
                    g_clients[slot].used = 0;
                }
            }
        }

        for (int k = 1; k < n && !exit_now; k++) {
            if (pfd[k].revents & (POLLIN | POLLHUP | POLLERR))
                exit_now = service_client(&g_clients[map[k]], token);
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
        drop_client(&g_clients[i]);
    close(lfd);
    if (g_unix_path[0] != '\0')
        unlink(g_unix_path);
    return 0;
}
