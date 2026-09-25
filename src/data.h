// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file data.h
 * @brief Cyclic data session: one datagram of inputs out and one of outputs in per bus cycle.
 *
 * The control thread opens and closes the session; the bus thread calls apply_outputs before
 * each exchange and publish_inputs after it. Neither blocks: the socket is non-blocking and the
 * session lock is only trylocked on the bus thread.
 */

#ifndef EDOG_DATA_H
#define EDOG_DATA_H

#include "config.h"
#include "log.h"

#include <stddef.h>

/** Output frames older than this are ignored and outputs go to the safe state (all zero). */
#define ECAT_DATA_TIMEOUT_MS 100

/** One-time init of the session lock. Call once per instance before any other data_* call. */
int ecat_data_init(ecat_master_instance_t *inst);
void ecat_data_destroy(ecat_master_instance_t *inst);

/**
 * @brief Open a session towards @p client_endpoint ("unix:/path" or "udp:127.0.0.1:port").
 *
 * Replaces any previous session. Fills @p endpoint_out with the address the client must send
 * its output frames to, and @p session_out with the random session id both sides put in every
 * frame. Only loopback UDP addresses are accepted.
 *
 * @return 0 on success, -1 with @p err filled on failure.
 */
int ecat_data_open(ecat_master_instance_t *inst, int master_index, const char *client_endpoint,
                   const char *state_dir, char *endpoint_out, size_t endpoint_size,
                   uint64_t *session_out, char *err, size_t err_size);

/** Close the session, if any. Outputs are zeroed on the next cycle. */
void ecat_data_close(ecat_master_instance_t *inst);

/** Bus thread, before the exchange: apply the newest valid output frame, or the safe state. */
void ecat_data_apply_outputs(ecat_master_instance_t *inst, int master_index);

/** Bus thread, after the exchange: send this cycle's inputs to the client. */
void ecat_data_publish_inputs(ecat_master_instance_t *inst, int master_index, int wkc,
                              bool operational, bool wkc_ok);

#endif /* EDOG_DATA_H */
