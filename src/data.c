// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file data.c
 * @brief Cyclic data session over a non-blocking datagram socket (AF_UNIX or loopback UDP).
 */

#include "data.h"
#include "layout.h"
#include "etherdog_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define UNIX_PREFIX "unix:"
#define UDP_PREFIX "udp:"

int ecat_data_init(ecat_master_instance_t *inst)
{
    ecat_data_session_t *d = &inst->data;
    memset(d, 0, sizeof(*d));
    d->fd = -1;
    return pthread_mutex_init(&d->lock, NULL) == 0 ? 0 : -1;
}

void ecat_data_destroy(ecat_master_instance_t *inst)
{
    ecat_data_close(inst);
    pthread_mutex_destroy(&inst->data.lock);
}

static uint64_t random_session(void)
{
    uint64_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, &v, sizeof(v));
        close(fd);
        if (n == (ssize_t)sizeof(v) && v != 0)
            return v;
    }
    /* Fallback: not secret, but still distinct per session. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    v = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 16);
    return v ? v : 1;
}

static bool is_loopback(const struct in_addr *a)
{
    return (ntohl(a->s_addr) >> 24) == 127;
}

/* Parse "unix:/path" or "udp:127.0.0.1:port" into a sockaddr. */
static int parse_endpoint(const char *spec, struct sockaddr_storage *out, socklen_t *out_len,
                          char *err, size_t err_size)
{
    memset(out, 0, sizeof(*out));
    if (strncmp(spec, UNIX_PREFIX, strlen(UNIX_PREFIX)) == 0) {
        const char *path = spec + strlen(UNIX_PREFIX);
        struct sockaddr_un *un = (struct sockaddr_un *)out;
        if (path[0] == '\0' || strlen(path) >= sizeof(un->sun_path)) {
            snprintf(err, err_size, "invalid unix endpoint path");
            return -1;
        }
        un->sun_family = AF_UNIX;
        snprintf(un->sun_path, sizeof(un->sun_path), "%s", path);
        *out_len = (socklen_t)sizeof(*un);
        return 0;
    }
    if (strncmp(spec, UDP_PREFIX, strlen(UDP_PREFIX)) == 0) {
        char host[64];
        const char *rest = spec + strlen(UDP_PREFIX);
        const char *colon = strrchr(rest, ':');
        if (colon == NULL || (size_t)(colon - rest) >= sizeof(host)) {
            snprintf(err, err_size, "invalid udp endpoint (expected udp:127.0.0.1:port)");
            return -1;
        }
        memcpy(host, rest, (size_t)(colon - rest));
        host[colon - rest] = '\0';
        char *end = NULL;
        long port = strtol(colon + 1, &end, 10);
        struct sockaddr_in *in = (struct sockaddr_in *)out;
        if (end == colon + 1 || *end != '\0' || port <= 0 || port > 65535 ||
            inet_pton(AF_INET, host, &in->sin_addr) != 1) {
            snprintf(err, err_size, "invalid udp endpoint (expected udp:127.0.0.1:port)");
            return -1;
        }
        if (!is_loopback(&in->sin_addr)) {
            snprintf(err, err_size, "udp endpoint must be a loopback address");
            return -1;
        }
        in->sin_family = AF_INET;
        in->sin_port = htons((uint16_t)port);
        *out_len = (socklen_t)sizeof(*in);
        return 0;
    }
    snprintf(err, err_size, "unsupported endpoint '%s' (use unix:<path> or udp:<ip>:<port>)",
             spec);
    return -1;
}

static bool same_peer(const ecat_data_session_t *d, const struct sockaddr_storage *src,
                      socklen_t src_len)
{
    const struct sockaddr_storage *peer = (const struct sockaddr_storage *)d->peer;
    if (src->ss_family != peer->ss_family)
        return false;
    if (src->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)src;
        const struct sockaddr_in *b = (const struct sockaddr_in *)peer;
        return a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
    }
    if (src->ss_family == AF_UNIX) {
        const struct sockaddr_un *a = (const struct sockaddr_un *)src;
        const struct sockaddr_un *b = (const struct sockaddr_un *)peer;
        size_t off = offsetof(struct sockaddr_un, sun_path);
        if ((size_t)src_len <= off)
            return false; /* unbound sender */
        return strncmp(a->sun_path, b->sun_path, sizeof(a->sun_path)) == 0;
    }
    return false;
}

/* Caller holds d->lock. */
static void close_locked(ecat_data_session_t *d)
{
    if (d->fd >= 0)
        close(d->fd);
    if (d->local_path[0] != '\0')
        unlink(d->local_path);
    d->fd = -1;
    d->local_path[0] = '\0';
    d->open = false;
    d->session = 0;
    d->rx_seen = false;
}

void ecat_data_close(ecat_master_instance_t *inst)
{
    ecat_data_session_t *d = &inst->data;
    pthread_mutex_lock(&d->lock);
    close_locked(d);
    pthread_mutex_unlock(&d->lock);
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int ecat_data_open(ecat_master_instance_t *inst, int master_index, const char *client_endpoint,
                   const char *state_dir, char *endpoint_out, size_t endpoint_size,
                   uint64_t *session_out, char *err, size_t err_size)
{
    struct sockaddr_storage peer;
    socklen_t peer_len = 0;
    if (client_endpoint == NULL ||
        parse_endpoint(client_endpoint, &peer, &peer_len, err, err_size) != 0)
        return -1;

    ecat_data_session_t *d = &inst->data;
    pthread_mutex_lock(&d->lock);
    close_locked(d);

    int fd = socket(peer.ss_family, SOCK_DGRAM, 0);
    if (fd < 0) {
        snprintf(err, err_size, "socket: %s", strerror(errno));
        goto fail;
    }
    if (set_nonblocking(fd) != 0) {
        snprintf(err, err_size, "fcntl: %s", strerror(errno));
        close(fd);
        goto fail;
    }

    if (peer.ss_family == AF_UNIX) {
        struct sockaddr_un local;
        memset(&local, 0, sizeof(local));
        local.sun_family = AF_UNIX;
        int n = snprintf(local.sun_path, sizeof(local.sun_path), "%s/etherdog-data-%d-%ld.sock",
                         state_dir, master_index, (long)getpid());
        if (n < 0 || (size_t)n >= sizeof(local.sun_path)) {
            snprintf(err, err_size, "state directory path too long for a socket");
            close(fd);
            goto fail;
        }
        unlink(local.sun_path);
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
            snprintf(err, err_size, "bind %s: %s", local.sun_path, strerror(errno));
            close(fd);
            goto fail;
        }
        chmod(local.sun_path, 0600);
        snprintf(d->local_path, sizeof(d->local_path), "%s", local.sun_path);
        snprintf(endpoint_out, endpoint_size, UNIX_PREFIX "%s", local.sun_path);
    } else {
        struct sockaddr_in local;
        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        socklen_t len = sizeof(local);
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0 ||
            getsockname(fd, (struct sockaddr *)&local, &len) != 0) {
            snprintf(err, err_size, "bind udp: %s", strerror(errno));
            close(fd);
            goto fail;
        }
        snprintf(endpoint_out, endpoint_size, UDP_PREFIX "127.0.0.1:%u",
                 (unsigned)ntohs(local.sin_port));
    }

    d->fd = fd;
    d->family = peer.ss_family;
    memcpy(d->peer, &peer, sizeof(peer) <= sizeof(d->peer) ? sizeof(peer) : sizeof(d->peer));
    d->peer_len = (uint32_t)peer_len;
    d->session = random_session();
    d->tx_seq = 0;
    d->rx_last_seq = 0;
    d->rx_seen = false;
    d->cycles_since_output = 0;
    d->outputs_zeroed = false;
    d->open = true;
    *session_out = d->session;
    pthread_mutex_unlock(&d->lock);
    return 0;

fail:
    pthread_mutex_unlock(&d->lock);
    return -1;
}

static uint32_t timeout_cycles(const ecat_master_instance_t *inst)
{
    int cycle_us = inst->config.master.cycle_time_us > 0 ? inst->config.master.cycle_time_us : 1000;
    uint32_t cycles = (uint32_t)((ECAT_DATA_TIMEOUT_MS * 1000) / cycle_us);
    return cycles < 3 ? 3 : cycles;
}

void ecat_data_apply_outputs(ecat_master_instance_t *inst, int master_index)
{
    ecat_data_session_t *d = &inst->data;
    uint8_t *region = ecat_layout_output_region(inst);
    uint32_t out_bytes = inst->layout.output_bytes;
    if (region == NULL)
        return;

    if (pthread_mutex_trylock(&d->lock) != 0)
        return; /* control thread is reconfiguring; keep last outputs one cycle */

    if (!d->open) {
        if (!d->outputs_zeroed) {
            memset(region, 0, out_bytes);
            d->outputs_zeroed = true;
        }
        pthread_mutex_unlock(&d->lock);
        return;
    }

    uint8_t buf[EDOG_FRAME_HEADER_SIZE + EDOG_FRAME_MAX_PAYLOAD];
    uint8_t latest[EDOG_FRAME_MAX_PAYLOAD];
    bool have_new = false;
    bool latest_valid = false;

    for (;;) {
        struct sockaddr_storage src;
        socklen_t src_len = sizeof(src);
        ssize_t n = recvfrom(d->fd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (n < 0)
            break; /* EAGAIN: drained */

        edog_frame_header_t h;
        if (!edog_frame_decode_header(buf, (size_t)n, &h) || h.kind != EDOG_FRAME_OUTPUTS ||
            h.master != (uint8_t)master_index || h.session != d->session ||
            h.payload_len != out_bytes || !same_peer(d, &src, src_len) ||
            (d->rx_seen && !edog_seq_newer(h.sequence, d->rx_last_seq))) {
            atomic_fetch_add_explicit(&d->frames_dropped, 1, memory_order_relaxed);
            continue;
        }
        d->rx_seen = true;
        d->rx_last_seq = h.sequence;
        latest_valid = (h.flags & EDOG_FLAG_VALID) != 0;
        if (latest_valid)
            memcpy(latest, buf + EDOG_FRAME_HEADER_SIZE, out_bytes);
        have_new = true;
        atomic_fetch_add_explicit(&d->frames_rx, 1, memory_order_relaxed);
    }

    if (have_new) {
        if (latest_valid) {
            memcpy(region, latest, out_bytes);
            d->outputs_zeroed = false;
        } else {
            memset(region, 0, out_bytes);
            d->outputs_zeroed = true;
        }
        d->cycles_since_output = 0;
    } else if (++d->cycles_since_output > timeout_cycles(inst) && !d->outputs_zeroed) {
        memset(region, 0, out_bytes);
        d->outputs_zeroed = true;
        atomic_fetch_add_explicit(&d->watchdog_trips, 1, memory_order_relaxed);
    }

    pthread_mutex_unlock(&d->lock);
}

void ecat_data_publish_inputs(ecat_master_instance_t *inst, int master_index, int wkc,
                              bool operational, bool wkc_ok)
{
    ecat_data_session_t *d = &inst->data;
    const uint8_t *region = ecat_layout_input_region(inst);
    uint32_t in_bytes = inst->layout.input_bytes;

    if (pthread_mutex_trylock(&d->lock) != 0)
        return;
    if (!d->open || d->fd < 0) {
        pthread_mutex_unlock(&d->lock);
        return;
    }

    uint8_t buf[EDOG_FRAME_HEADER_SIZE + EDOG_FRAME_MAX_PAYLOAD];
    edog_frame_header_t h = {
        .version = EDOG_PROTOCOL_VERSION,
        .kind = EDOG_FRAME_INPUTS,
        .master = (uint8_t)master_index,
        .flags = (uint8_t)((operational ? EDOG_FLAG_VALID : 0) | (wkc_ok ? EDOG_FLAG_WKC_OK : 0)),
        .session = d->session,
        .sequence = ++d->tx_seq,
        .payload_len = (uint16_t)in_bytes,
        .wkc = (uint16_t)(wkc < 0 ? 0 : wkc),
    };
    edog_frame_encode_header(buf, &h);
    if (region != NULL && in_bytes > 0)
        memcpy(buf + EDOG_FRAME_HEADER_SIZE, region, in_bytes);

    ssize_t n = sendto(d->fd, buf, EDOG_FRAME_HEADER_SIZE + in_bytes, 0,
                       (const struct sockaddr *)d->peer, (socklen_t)d->peer_len);
    if (n < 0)
        atomic_fetch_add_explicit(&d->frames_dropped, 1, memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&d->frames_tx, 1, memory_order_relaxed);

    pthread_mutex_unlock(&d->lock);
}
