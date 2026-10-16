// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file iface_state.h
 * @brief Per-interface NIC-tuning state manager.
 *
 * Saves the NIC's pre-EtherCAT settings (ethtool -c / -k coalescing
 * and offloads), applies the low-latency tuning the bus thread needs
 * (rx/tx-usecs=0, GRO/GSO/TSO off), and persists the captured "before"
 * values to the state directory so the next start after a crash can revert them.
 *
 * Persistence file:  <state dir>/ecat_iface_<iface>.state
 *
 * Also requests promiscuous mode per SOEM socket (Linux), which needs no
 * persistence: the kernel drops the request when the socket closes.
 */

#ifndef ETHERCAT_IFACE_STATE_H
#define ETHERCAT_IFACE_STATE_H

#include "config.h"   /* ecat_iface_state_t */
#include "log.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default directory for the crash-recovery NIC state files. */
#ifndef EDOG_DEFAULT_STATE_DIR
#define EDOG_DEFAULT_STATE_DIR "/run/etherdog"
#endif

/** Override the state directory (call before the first apply). */
void ecat_iface_state_set_dir(const char *dir);

/**
 * @brief Apply low-latency NIC tuning.
 *
 * Sequence:
 *   1. Migrate the legacy NIC state file (older format) and revert
 *      anything it describes.
 *   2. Recover from the unified state file if a previous run crashed.
 *   3. Capture current NIC settings (ethtool -c / -k).
 *   4. Apply low-latency settings (rx/tx-usecs = 0, GRO/GSO/TSO off).
 *   5. Persist the captured "before" values to the state directory so a crash
 *      lets the next start undo what we just applied.
 *
 * On non-Linux platforms this is a no-op.
 *
 * @param state  Per-instance state (zeroed by caller; populated here).
 * @param iface  Interface name from the master config.
 * @param logger Logger.
 */
void ecat_iface_state_apply(ecat_iface_state_t *state, const char *iface,
                            edog_logger_t *logger);

/**
 * @brief Revert exactly what apply applied.
 *
 * Reads the in-memory flags (no disk involvement) to undo only the
 * changes that this instance made, then deletes the persistence file.
 * Safe to call when nothing was applied (no-op).  Safe to call after a
 * partially-failed apply (skips fields that were never set).
 */
void ecat_iface_state_revert(ecat_iface_state_t *state, edog_logger_t *logger);

/**
 * @brief Request promiscuous mode on @p iface for as long as the SOEM socket stays open.
 *
 * Uses PACKET_MR_PROMISC on the raw socket of @p ctx. It needs no CAP_NET_ADMIN, and the
 * kernel drops the request when ecx_close() closes the socket, so nothing is left to
 * revert. EtherCAT replies are broadcast, so a failure only logs a warning.
 *
 * On non-Linux platforms this is a no-op that returns 0.
 *
 * @param ctx    SOEM context after a successful ecx_init() on @p iface.
 * @param iface  Interface name passed to ecx_init().
 * @param logger Logger.
 * @return 0 when promiscuous mode was requested, -1 otherwise.
 */
int ecat_iface_request_promisc(ecx_contextt *ctx, const char *iface, edog_logger_t *logger);

#ifdef __cplusplus
}
#endif

#endif /* ETHERCAT_IFACE_STATE_H */
