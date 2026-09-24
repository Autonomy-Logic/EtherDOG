// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file dgram.h
 * @brief Non-blocking datagram endpoints ("unix:/path" or "udp:127.0.0.1:port").
 *
 * Kept apart from the SOEM headers: on MSYS2 they pull in winsock2, which conflicts with the
 * POSIX socket headers this file uses.
 */

#ifndef EDOG_DGRAM_H
#define EDOG_DGRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/** Room for a struct sockaddr_storage. */
#define EDOG_DGRAM_ADDR_MAX 128

typedef struct {
    uint8_t  addr[EDOG_DGRAM_ADDR_MAX];
    uint32_t len;
    int      family;
} edog_dgram_peer_t;

/** Parse a client endpoint. Only loopback UDP addresses are accepted. 0 on success. */
int edog_dgram_parse(const char *spec, edog_dgram_peer_t *peer, char *err, size_t err_size);

/**
 * @brief Open a non-blocking socket of @p peer's family and bind it locally.
 *
 * AF_UNIX binds "<state_dir>/etherdog-data-<master>-<pid>.sock" (mode 0600) and stores it in
 * @p local_path; UDP binds an ephemeral loopback port. @p endpoint_out gets the bound address.
 *
 * @return the socket, or -1 with @p err filled.
 */
int edog_dgram_open(const edog_dgram_peer_t *peer, const char *state_dir, int master_index,
                    char *local_path, size_t local_path_size, char *endpoint_out,
                    size_t endpoint_size, char *err, size_t err_size);

/** Close @p fd and unlink @p local_path when set. */
void edog_dgram_close(int fd, const char *local_path);

/** Receive one datagram. @p from_peer tells whether it came from @p peer. -1 when drained. */
ssize_t edog_dgram_recv(int fd, void *buf, size_t size, const edog_dgram_peer_t *peer,
                        bool *from_peer);

/** Send one datagram to @p peer. */
ssize_t edog_dgram_send(int fd, const void *buf, size_t size, const edog_dgram_peer_t *peer);

#endif /* EDOG_DGRAM_H */
