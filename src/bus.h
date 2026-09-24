// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file bus.h
 * @brief EtherCAT masters: configuration, start/stop and the JSON command set.
 *
 * Every function here is called from the control thread only. The bus and monitor threads
 * are owned by this module and never call back into it.
 */

#ifndef EDOG_BUS_H
#define EDOG_BUS_H

#include <stdbool.h>
#include <stddef.h>

/** Set up the module logger. Call once at process start. */
void ecat_bus_init(void);

/** Directory for data sockets and NIC recovery files (default /run/etherdog). */
void ecat_bus_set_state_dir(const char *dir);

/**
 * @brief Load a bus configuration file, replacing the current one.
 *
 * Refused while any master is running. A NULL or empty @p path clears the configuration.
 * @return 0 on success, -1 with @p err filled.
 */
int ecat_bus_configure(const char *path, char *err, size_t err_size);

/** Bring every configured master to OPERATIONAL. Returns how many run, or -1 if none. */
int ecat_bus_start(char *err, size_t err_size);

/** Stop every master: outputs zeroed (safe_close), slaves to INIT, sessions closed. */
void ecat_bus_stop(void);

/** Stop and free everything. */
void ecat_bus_shutdown(void);

/** True while any master is between SCANNING and RECOVERING. */
bool ecat_bus_any_active(void);

/**
 * @brief Run one JSON command ({"command": "...", "params": {...}}).
 *
 * The response is always a JSON object written into @p response; failures carry an
 * "error" key. Command set: docs/PROTOCOL.md.
 *
 * @return 0 on success, -1 on failure.
 */
int ecat_bus_command(const char *command_json, char *response, size_t response_size);

#endif /* EDOG_BUS_H */
