// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file dgram.c
 * @brief Non-blocking datagram endpoints over AF_UNIX or loopback UDP.
 */

#include "dgram.h"

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
#include <unistd.h>

#define UNIX_PREFIX "unix:"
#define UDP_PREFIX "udp:"

_Static_assert(sizeof(struct sockaddr_storage) <= EDOG_DGRAM_ADDR_MAX,
               "EDOG_DGRAM_ADDR_MAX too small for sockaddr_storage");

static bool is_loopback(const struct in_addr *a)
{
    return (ntohl(a->s_addr) >> 24) == 127;
}

int edog_dgram_parse(const char *spec, edog_dgram_peer_t *peer, char *err, size_t err_size)
{
    memset(peer, 0, sizeof(*peer));
    if (strncmp(spec, UNIX_PREFIX, strlen(UNIX_PREFIX)) == 0) {
#if defined(__CYGWIN__)
        /* Cygwin AF_UNIX datagrams carry no sender path, so the peer check would drop every frame */
        snprintf(err, err_size, "unix data endpoints are not supported on Windows; use udp");
        return -1;
#endif
        const char *path = spec + strlen(UNIX_PREFIX);
        struct sockaddr_un *un = (struct sockaddr_un *)peer->addr;
        if (path[0] == '\0' || strlen(path) >= sizeof(un->sun_path)) {
            snprintf(err, err_size, "invalid unix endpoint path");
            return -1;
        }
        un->sun_family = AF_UNIX;
        snprintf(un->sun_path, sizeof(un->sun_path), "%s", path);
        peer->len = (uint32_t)sizeof(*un);
        peer->family = AF_UNIX;
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
        struct sockaddr_in *in = (struct sockaddr_in *)peer->addr;
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
        peer->len = (uint32_t)sizeof(*in);
        peer->family = AF_INET;
        return 0;
    }
    snprintf(err, err_size, "unsupported endpoint '%s' (use unix:<path> or udp:<ip>:<port>)",
             spec);
    return -1;
}

static bool same_peer(const edog_dgram_peer_t *peer, const struct sockaddr_storage *src,
                      socklen_t src_len)
{
    const struct sockaddr_storage *p = (const struct sockaddr_storage *)peer->addr;
    if (src->ss_family != p->ss_family)
        return false;
    if (src->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)src;
        const struct sockaddr_in *b = (const struct sockaddr_in *)p;
        return a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
    }
    if (src->ss_family == AF_UNIX) {
        const struct sockaddr_un *a = (const struct sockaddr_un *)src;
        const struct sockaddr_un *b = (const struct sockaddr_un *)p;
        size_t off = offsetof(struct sockaddr_un, sun_path);
        if ((size_t)src_len <= off)
            return false; /* unbound sender */
        return strncmp(a->sun_path, b->sun_path, sizeof(a->sun_path)) == 0;
    }
    return false;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int edog_dgram_open(const edog_dgram_peer_t *peer, const char *state_dir, int master_index,
                    char *local_path, size_t local_path_size, char *endpoint_out,
                    size_t endpoint_size, char *err, size_t err_size)
{
    local_path[0] = '\0';
    int fd = socket(peer->family, SOCK_DGRAM, 0);
    if (fd < 0) {
        snprintf(err, err_size, "socket: %s", strerror(errno));
        return -1;
    }
    if (set_nonblocking(fd) != 0) {
        snprintf(err, err_size, "fcntl: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (peer->family == AF_UNIX) {
        struct sockaddr_un local;
        memset(&local, 0, sizeof(local));
        local.sun_family = AF_UNIX;
        int n = snprintf(local.sun_path, sizeof(local.sun_path), "%s/etherdog-data-%d-%ld.sock",
                         state_dir, master_index, (long)getpid());
        if (n < 0 || (size_t)n >= sizeof(local.sun_path) || (size_t)n >= local_path_size) {
            snprintf(err, err_size, "state directory path too long for a socket");
            close(fd);
            return -1;
        }
        unlink(local.sun_path);
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
            snprintf(err, err_size, "bind %s: %s", local.sun_path, strerror(errno));
            close(fd);
            return -1;
        }
        chmod(local.sun_path, 0600);
        snprintf(local_path, local_path_size, "%s", local.sun_path);
        snprintf(endpoint_out, endpoint_size, UNIX_PREFIX "%s", local.sun_path);
        return fd;
    }

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
        return -1;
    }
    snprintf(endpoint_out, endpoint_size, UDP_PREFIX "127.0.0.1:%u",
             (unsigned)ntohs(local.sin_port));
    return fd;
}

void edog_dgram_close(int fd, const char *local_path)
{
    if (fd >= 0)
        close(fd);
    if (local_path != NULL && local_path[0] != '\0')
        unlink(local_path);
}

ssize_t edog_dgram_recv(int fd, void *buf, size_t size, const edog_dgram_peer_t *peer,
                        bool *from_peer)
{
    struct sockaddr_storage src;
    socklen_t src_len = sizeof(src);
    ssize_t n = recvfrom(fd, buf, size, 0, (struct sockaddr *)&src, &src_len);
    *from_peer = n >= 0 && same_peer(peer, &src, src_len);
    return n;
}

ssize_t edog_dgram_send(int fd, const void *buf, size_t size, const edog_dgram_peer_t *peer)
{
    return sendto(fd, buf, size, 0, (const struct sockaddr *)peer->addr, (socklen_t)peer->len);
}
