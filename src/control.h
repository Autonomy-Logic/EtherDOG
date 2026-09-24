// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file control.h
 * @brief Control server: newline-delimited JSON commands over a stream socket.
 *
 * Listens on "unix:/path" (mode 0600) or "tcp:127.0.0.1:port". On a unix socket every peer
 * must run as EtherDOG's own user, root, or an allowed uid (SO_PEERCRED). A tcp socket has no
 * peer identity, so it requires a token. When a token is configured every connection must open
 * with {"command":"hello","params":{"token":"..."}}. Requests are handled one at a time.
 */

#ifndef EDOG_CONTROL_H
#define EDOG_CONTROL_H

#include <signal.h>
#include <sys/types.h>

/**
 * @brief Serve until @p stop becomes non-zero or a "shutdown" command arrives.
 * @param listen_spec "unix:<path>" or "tcp:<loopback ip>:<port>"
 * @param token       shared secret, or NULL/"" for none (not allowed with tcp)
 * @param allow_uids  extra uids allowed on a unix socket, besides EtherDOG's own and root
 * @return 0 on a clean exit, -1 if the socket could not be opened.
 */
int edog_control_run(const char *listen_spec, const char *token, const uid_t *allow_uids,
                     int allow_count, volatile sig_atomic_t *stop);

#endif /* EDOG_CONTROL_H */
