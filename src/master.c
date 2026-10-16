// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file master.c
 * @brief EtherCAT Master SOEM Wrapper Implementation
 *
 * Wraps the SOEM library to provide high-level EtherCAT master operations:
 * network initialization, slave scanning, topology validation against
 * the JSON configuration, SDO writes, state machine management, and
 * slave recovery.
 *
 * Uses the ecx_* context-based API from SOEM 2.x.
 */

#include "master.h"
#include "proc.h"
#include "iface_state.h"
#include "soem/soem.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Low-latency socket option for the SOEM raw socket (Linux only).
 * Per-interface NIC tuning (ethtool coalescing/offloads) lives in
 * ethercat_iface_state.{c,h}. */
#if !defined(__CYGWIN__) && !defined(_WIN32)
#include <sys/socket.h>
#define ECAT_BUSY_POLL_US 50
#endif

/* Zero-output frames sent on close until one comes back */
#define ECAT_ZERO_OUTPUT_ATTEMPTS 3

/* encode_sdo_value copies host bytes into the little-endian wire buffer; a big-endian port needs
 * htole32/htole64 there. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "EtherCAT SDO encoding requires a little-endian host");
#endif

/*
 * =============================================================================
 * SOEM Context and IO Map
 * =============================================================================
 */

/** Minimum retry count when polling for OPERATIONAL state.  The actual
 *  retry count scales with min_sm_wd / total budget so the cadence stays
 *  inside the smallest configured SM watchdog (see transition_to_op). */
#define ECAT_OP_POLL_RETRIES 10

/*
 * =============================================================================
 * Topology Validation
 * =============================================================================
 */

int ecat_master_validate_topology(ecat_master_instance_t *inst, edog_logger_t *logger)
{
    const ecat_config_t *config = &inst->config;
    int found_count = inst->ecx_context.slavecount;

    if (found_count != config->slave_count) {
        edog_log_error(logger,
            "Topology mismatch: expected %d slaves, found %d on the bus",
            config->slave_count, found_count);
        return -1;
    }

    for (int i = 0; i < config->slave_count; i++) {
        const ecat_slave_t *expected = &config->slaves[i];
        int pos = expected->position;

        if (pos < 1 || pos > found_count) {
            edog_log_error(logger,
                "Slave %d: position %d is out of range (1-%d)",
                i, pos, found_count);
            return -1;
        }

        ec_slavet *found = &inst->ecx_context.slavelist[pos];

        /* Vendor ID check (can be disabled per slave via startup_checks) */
        if (expected->startup_checks.check_vendor_id) {
            if (found->eep_man != expected->vendor_id) {
                edog_log_error(logger,
                    "Slave %d (%s) at position %d: vendor_id mismatch - "
                    "expected 0x%08X, found 0x%08X",
                    i, expected->name, pos,
                    expected->vendor_id, found->eep_man);
                return -1;
            }
        } else {
            edog_log_debug(logger,
                "Slave %d (%s) at position %d: vendor_id check disabled",
                i, expected->name, pos);
        }

        /* Product code check (can be disabled per slave via startup_checks) */
        if (expected->startup_checks.check_product_code) {
            if (found->eep_id != expected->product_code) {
                edog_log_error(logger,
                    "Slave %d (%s) at position %d: product_code mismatch - "
                    "expected 0x%08X, found 0x%08X",
                    i, expected->name, pos,
                    expected->product_code, found->eep_id);
                return -1;
            }
        } else {
            edog_log_debug(logger,
                "Slave %d (%s) at position %d: product_code check disabled",
                i, expected->name, pos);
        }

        edog_log_debug(logger,
            "Slave %d (%s) at position %d: topology OK "
            "(vendor=0x%08X, product=0x%08X)",
            i, expected->name, pos,
            found->eep_man, found->eep_id);
    }

    edog_log_info(logger, "Topology validation passed: %d slaves match configuration",
                       config->slave_count);
    return 0;
}

void ecat_soem_release_failed_init(ecx_contextt *ctx)
{
    /* ecx_init creates the mailbox pool mutex before it opens the NIC; ecx_close would also
     * close a socket that was never opened. */
    osal_mutex_destroy(ctx->mbxpool.mbxmutex);
    ctx->mbxpool.mbxmutex = NULL;
}

/*
 * =============================================================================
 * Phase 1: Open Interface, Scan Bus, Validate Topology
 * =============================================================================
 */

int ecat_master_open_and_scan(ecat_master_instance_t *inst, edog_logger_t *logger)
{
    const ecat_config_t *config = &inst->config;

    /* Zero-initialize the SOEM context before use */
    memset(&inst->ecx_context, 0, sizeof(inst->ecx_context));

    /* iomap and slaves_snapshot are allocated per start; release anything a failed start left. */
    free(inst->iomap);
    inst->iomap = NULL;
    inst->iomap_capacity = 0;
    inst->iomap_used_size = 0;
    free(inst->slaves_snapshot);
    inst->slaves_snapshot = NULL;
    inst->slaves_snapshot_capacity = 0;
    inst->slaves_snapshot_count = 0;

    /* Step 0: Apply per-iface external state (NIC tuning + IP-stack
     * isolation).  Reverted in ecat_master_close. */
    ecat_iface_state_apply(&inst->iface_state, config->master.interface, logger);

    /* Step 1: Initialize SOEM on the configured network interface */
    edog_log_info(logger, "Opening network interface: %s", config->master.interface);

    if (!ecx_init(&inst->ecx_context, config->master.interface)) {
        ecat_soem_release_failed_init(&inst->ecx_context);
#if defined(__CYGWIN__) || defined(_WIN32)
        edog_log_error(logger,
            "Failed to initialize EtherCAT interface '%s'. "
            "Verify that Npcap (https://npcap.com) is installed and "
            "the interface name matches a network adapter (use "
            "'ipconfig' or Npcap's WlanHelper to list adapters).",
            config->master.interface);
#else
        edog_log_error(logger,
            "Failed to initialize EtherCAT interface '%s'. "
            "Check that the interface exists and the process has "
            "CAP_NET_RAW capability (or is running as root).",
            config->master.interface);
#endif
        return -1;
    }

    inst->soem_initialized = 1;
    edog_log_info(logger, "Network interface opened successfully");

    /* Enable SO_BUSY_POLL on the SOEM raw socket.
     * This makes recvfrom() spin-poll the NIC driver instead of sleeping,
     * eliminating ~5-10us of scheduler wakeup latency per exchange. */
#ifdef ECAT_BUSY_POLL_US
    {
        int busy_us = ECAT_BUSY_POLL_US;
        int sockfd = inst->ecx_context.port.sockhandle;
        if (sockfd >= 0) {
            if (setsockopt(sockfd, SOL_SOCKET, SO_BUSY_POLL,
                           &busy_us, sizeof(busy_us)) == 0) {
                edog_log_info(logger,
                    "SO_BUSY_POLL enabled on socket (poll=%d us)", busy_us);
            } else {
                edog_log_debug(logger,
                    "SO_BUSY_POLL not supported (kernel may need CONFIG_NET_RX_BUSY_POLL)");
            }
        }
    }
#endif

    /* Step 2: Scan the bus and enumerate slaves */
    edog_log_info(logger, "Scanning EtherCAT bus...");

    if (ecx_config_init(&inst->ecx_context) <= 0) {
        edog_log_error(logger,
            "No EtherCAT slaves found on interface '%s'. "
            "Check cable connections and slave power.",
            config->master.interface);
        ecx_close(&inst->ecx_context);
        inst->soem_initialized = 0;
        return -1;
    }

    edog_log_info(logger, "Found %d slave(s) on the bus", inst->ecx_context.slavecount);

    /* Log discovered slaves */
    for (int i = 1; i <= inst->ecx_context.slavecount; i++) {
        ec_slavet *slave = &inst->ecx_context.slavelist[i];
        edog_log_info(logger,
            "  [%d] %s - vendor=0x%08X, product=0x%08X, rev=0x%08X",
            i, slave->name, slave->eep_man, slave->eep_id, slave->eep_rev);
    }

    /* iomap (from the configured PDOs, grown before mapping) and slaves_snapshot (from
     * config.slave_count); both freed by ecat_master_close. */
    if (ecat_master_instance_alloc_iomap(inst) != 0) {
        ecx_close(&inst->ecx_context);
        inst->soem_initialized = 0;
        return -1;
    }
    if (ecat_master_instance_alloc_snapshot(inst) != 0) {
        ecx_close(&inst->ecx_context);
        inst->soem_initialized = 0;
        return -1;
    }

    /* Step 3: Validate topology against JSON configuration */
    if (ecat_master_validate_topology(inst, logger) != 0) {
        edog_log_error(logger,
            "Topology validation failed - aborting master initialization");
        ecx_close(&inst->ecx_context);
        inst->soem_initialized = 0;
        return -1;
    }

    /* Step 4: wait for PRE-OP (needed for SDOs); ecx_config_init only requests it. Longest
     * configured init_to_preop timeout. */
    edog_log_info(logger, "Waiting for slaves to reach PRE-OP state...");

    int max_preop_timeout_us = 0;
    for (int i = 0; i < config->slave_count; i++) {
        int t_us = config->slaves[i].timeouts.init_to_preop_timeout_ms * 1000;
        if (t_us > max_preop_timeout_us)
            max_preop_timeout_us = t_us;
    }
    if (max_preop_timeout_us == 0)
        max_preop_timeout_us = EC_TIMEOUTSTATE * 4;
    edog_log_debug(logger, "Using INIT->PRE-OP timeout: %d us", max_preop_timeout_us);

    ecx_statecheck(&inst->ecx_context, 0, EC_STATE_PRE_OP, max_preop_timeout_us);
    ecx_readstate(&inst->ecx_context);

    int all_preop = 1;
    for (int i = 1; i <= inst->ecx_context.slavecount; i++) {
        ec_slavet *slave = &inst->ecx_context.slavelist[i];
        if (slave->state < EC_STATE_PRE_OP) {
            edog_log_error(logger,
                "Slave %d (%s) failed to reach PRE-OP (state=0x%04X, ALstatus=0x%04X)",
                i, slave->name, slave->state, slave->ALstatuscode);
            all_preop = 0;
        }
    }

    if (!all_preop) {
        edog_log_error(logger, "Not all slaves reached PRE-OP - aborting");
        ecx_close(&inst->ecx_context);
        inst->soem_initialized = 0;
        return -1;
    }

    edog_log_info(logger, "All slaves in PRE-OP state");

    return 0;
}

/*
 * =============================================================================
 * Phase 2: SDO Configuration
 * =============================================================================
 */

/**
 * @brief Encode a double-typed SDO value into wire bytes for the target type.
 *
 * Three paths cover all 11 supported types: REAL32, REAL64, and integer.
 * Integer types share a common int64_t cast and a memcpy of the LSBs --
 * parse_sdo's range check guarantees the cast is in-range, so no UB on
 * the truncation.  Host little-endian is enforced at file scope by the
 * _Static_assert above; a future big-endian port fails the build there.
 *
 * @param dt     Target wire type (must be a valid known type)
 * @param in     Source value (already range-validated by the parser)
 * @param out    Output buffer, at least @p ecat_data_type_size(dt) bytes
 * @return Number of bytes written, or 0 if @p dt is unknown/PAD
 */
static int encode_sdo_value(ecat_data_type_t dt, double in, uint8_t out[8])
{
    int sz = ecat_data_type_size(dt);
    if (sz <= 0)
        return 0;

    memset(out, 0, 8);

    if (dt == ECAT_DTYPE_REAL32) {
        float v = (float)in;
        memcpy(out, &v, sizeof(v));
        return 4;
    }
    if (dt == ECAT_DTYPE_REAL64) {
        memcpy(out, &in, sizeof(in));
        return 8;
    }

    /* Integer path (BOOL/INT8/UINT8/.../UINT64): one cast to int64_t,
     * then memcpy the low @p sz bytes -- little-endian on supported hosts. */
    int64_t i = (int64_t)in;
    memcpy(out, &i, (size_t)sz);
    return sz;
}

int ecat_master_write_sdos(ecat_master_instance_t *inst, int slave_pos,
                           const ecat_sdo_config_t *sdos,
                           int sdo_count, int sdo_timeout_ms,
                           edog_logger_t *logger)
{
    if (!inst->soem_initialized) {
        edog_log_error(logger, "Cannot write SDOs: SOEM not initialized");
        return -1;
    }

    if (slave_pos < 1 || slave_pos > inst->ecx_context.slavecount) {
        edog_log_error(logger, "Invalid slave position %d for SDO write", slave_pos);
        return -1;
    }

    if (sdo_count == 0)
        return 0;

    /* Slaves without CoE mailbox cannot accept SDO writes; refuse early
     * with a clear message instead of letting every ecx_SDOwrite return
     * wkc=0. */
    if ((inst->ecx_context.slavelist[slave_pos].mbx_proto & 0x04) == 0) {
        edog_log_error(logger,
            "Slave %d: %d SDO(s) configured but slave does not support CoE mailbox",
            slave_pos, sdo_count);
        return -1;
    }

    int written = 0;

    for (int i = 0; i < sdo_count; i++) {
        const ecat_sdo_config_t *sdo = &sdos[i];

        /* Parse index from hex string */
        uint16_t index = (uint16_t)strtol(sdo->index, NULL, 16);

        /* value_bytes overrides the numeric value; complete_access writes with CA=TRUE. */
        const uint8_t *wire_buf;
        int size;
        uint8_t numeric_buf[8];

        if (sdo->value_bytes != NULL && sdo->value_bytes_len > 0) {
            wire_buf = sdo->value_bytes;
            if (sdo->value_bytes_len > INT_MAX) {
                edog_log_error(logger,
                    "Slave %d SDO 0x%04X:%d: byte payload too large (%zu bytes)",
                    slave_pos, index, sdo->subindex, sdo->value_bytes_len);
                continue;
            }
            size = (int)sdo->value_bytes_len;
            edog_log_debug(logger,
                "Slave %d: writing SDO 0x%04X:%d (byte-string, %d bytes%s)",
                slave_pos, index, sdo->subindex, size,
                sdo->complete_access ? ", CA" : "");
        } else {
            /* parse_sdo rejects UNKNOWN/PAD so encode_sdo_value() returning
             * 0 here is a parser regression rather than user input -- skip
             * the SDO defensively rather than crash. */
            ecat_data_type_t dt = sdo->parsed_type;
            int encoded = encode_sdo_value(dt, sdo->value, numeric_buf);
            if (encoded <= 0) {
                edog_log_error(logger,
                    "Slave %d SDO 0x%04X:%d: unknown data type '%s' -- skipping (parser regression?)",
                    slave_pos, index, sdo->subindex,
                    ecat_data_type_to_string(dt));
                continue;
            }
            wire_buf = numeric_buf;
            size = encoded;
            const char *dt_name = ecat_data_type_to_string(dt);
            if (dt == ECAT_DTYPE_REAL32 || dt == ECAT_DTYPE_REAL64) {
                edog_log_debug(logger,
                    "Slave %d: writing SDO 0x%04X:%d = %g (%s, %d bytes%s)",
                    slave_pos, index, sdo->subindex, sdo->value, dt_name, size,
                    sdo->complete_access ? ", CA" : "");
            } else {
                edog_log_debug(logger,
                    "Slave %d: writing SDO 0x%04X:%d = %lld (%s, %d bytes%s)",
                    slave_pos, index, sdo->subindex, (long long)(int64_t)sdo->value,
                    dt_name, size, sdo->complete_access ? ", CA" : "");
            }
        }

        /* Use per-slave SDO timeout if configured, otherwise SOEM default */
        int sdo_timeout_us = (sdo_timeout_ms > 0) ? (sdo_timeout_ms * 1000) : EC_TIMEOUTRXM;

        int wkc = ecx_SDOwrite(&inst->ecx_context, (uint16)slave_pos,
                                index, sdo->subindex,
                                sdo->complete_access ? TRUE : FALSE,
                                size, (void *)wire_buf, sdo_timeout_us);

        if (wkc <= 0) {
            edog_log_warn(logger,
                "Slave %d SDO 0x%04X:%d write failed (wkc=%d, name='%s')",
                slave_pos, index, sdo->subindex, wkc, sdo->name);
        } else {
            edog_log_debug(logger,
                "Slave %d SDO 0x%04X:%d write OK (name='%s')",
                slave_pos, index, sdo->subindex, sdo->name);
            written++;
        }
    }

    if (written < sdo_count) {
        edog_log_warn(logger,
            "Slave %d: only %d/%d SDOs written successfully",
            slave_pos, written, sdo_count);
        return -1;
    }
    edog_log_info(logger, "Slave %d: %d/%d SDOs written successfully",
                       slave_pos, written, sdo_count);
    return 0;
}

/*
 * =============================================================================
 * Phase 3: Process Data Mapping + Distributed Clocks
 * =============================================================================
 */

/**
 * @brief Configure watchdog timers for a single slave via register writes.
 *
 * EtherCAT watchdog registers (addressed by configured address):
 *   0x0400 (2 bytes) - Watchdog divider (default 0x09C2 = 2498)
 *   0x0402 (2 bytes) - PDI watchdog time (in watchdog divider ticks)
 *   0x0420 (2 bytes) - SM watchdog time  (in watchdog divider ticks)
 *
 * Default divider 0x09C2 = 2498 -> (2498+2)*25ns = 62.5us per tick.
 * To set watchdog to X ms: ticks = X * 1000 / 62.5 = X * 16
 *
 * SM watchdog is treated as a critical configuration when @p strict is
 * true: if the FPWR returns wkc<=0, the slave would run with the
 * EEPROM-default watchdog (often disabled), defeating the operator's
 * intent.  PDI watchdog is always best-effort -- many slaves do not
 * support that register and return wkc=0 legitimately.
 *
 * @param inst      Master instance
 * @param slave_pos 1-based slave position on the bus
 * @param wd        Watchdog configuration
 * @param strict    Abort startup on SM watchdog write failure
 * @param logger    Logger
 * @return 0 on success, -1 on SM watchdog write failure with @p strict
 */
static int ecat_master_configure_watchdog(ecat_master_instance_t *inst, int slave_pos,
                                          const ecat_watchdog_t *wd,
                                          bool strict,
                                          edog_logger_t *logger)
{
    /* Maximum watchdog timeout in ms that fits in a uint16_t register
     * with the default divider (1 tick = 62.5 us -> ticks = ms * 16).
     * 65535 / 16 = 4095.9 ms */
    const int max_watchdog_ms = UINT16_MAX / 16;

    uint16_t configadr = inst->ecx_context.slavelist[slave_pos].configadr;
    int wkc;

    /* SM watchdog register 0x0420 - only write if explicitly enabled. */
    if (wd->sm_watchdog_enabled) {
        uint16_t sm_wd_ticks = 0;
        if (wd->sm_watchdog_ms > 0) {
            int clamped_ms = wd->sm_watchdog_ms;
            if (clamped_ms > max_watchdog_ms) {
                edog_log_warn(logger,
                    "Slave %d: SM watchdog %d ms exceeds max %d ms, clamping",
                    slave_pos, wd->sm_watchdog_ms, max_watchdog_ms);
                clamped_ms = max_watchdog_ms;
            }
            sm_wd_ticks = (uint16_t)(clamped_ms * 16);
        }
        wkc = ecx_FPWR(&inst->ecx_context.port, configadr, 0x0420,
                            sizeof(sm_wd_ticks), &sm_wd_ticks, EC_TIMEOUTRET);
        if (wkc <= 0) {
            if (strict) {
                edog_log_error(logger,
                    "Slave %d: failed to write SM watchdog register 0x0420 (wkc=%d) -- "
                    "slave would run with EEPROM-default watchdog (often disabled)",
                    slave_pos, wkc);
                return -1;
            }
            edog_log_warn(logger,
                "Slave %d: SM watchdog write failed (wkc=%d), strict_sdo=false -- continuing",
                slave_pos, wkc);
        } else {
            edog_log_debug(logger,
                "Slave %d: SM watchdog enabled (ticks=%u, ~%d ms)",
                slave_pos, sm_wd_ticks, wd->sm_watchdog_ms);
        }
    } else {
        edog_log_debug(logger,
            "Slave %d: SM watchdog disabled, skipping register write",
            slave_pos);
    }

    /* PDI watchdog register 0x0402 - many slaves do not support this
     * register and return wkc=0 legitimately, so always best-effort. */
    if (wd->pdi_watchdog_enabled) {
        uint16_t pdi_wd_ticks = 0;
        if (wd->pdi_watchdog_ms > 0) {
            int clamped_ms = wd->pdi_watchdog_ms;
            if (clamped_ms > max_watchdog_ms) {
                edog_log_warn(logger,
                    "Slave %d: PDI watchdog %d ms exceeds max %d ms, clamping",
                    slave_pos, wd->pdi_watchdog_ms, max_watchdog_ms);
                clamped_ms = max_watchdog_ms;
            }
            pdi_wd_ticks = (uint16_t)(clamped_ms * 16);
        }
        wkc = ecx_FPWR(&inst->ecx_context.port, configadr, 0x0402,
                        sizeof(pdi_wd_ticks), &pdi_wd_ticks, EC_TIMEOUTRET);
        if (wkc <= 0) {
            edog_log_warn(logger,
                "Slave %d: PDI watchdog write returned wkc=%d (many slaves do not "
                "support this register -- typically benign)",
                slave_pos, wkc);
        } else {
            edog_log_debug(logger,
                "Slave %d: PDI watchdog enabled (ticks=%u, ~%d ms)",
                slave_pos, pdi_wd_ticks, wd->pdi_watchdog_ms);
        }
    } else {
        edog_log_debug(logger,
            "Slave %d: PDI watchdog disabled, skipping register write",
            slave_pos);
    }

    return 0;
}

/**
 * @brief Configure Distributed Clocks per slave based on JSON configuration.
 *
 * First calls ecx_configdc() to discover DC-capable slaves and measure
 * propagation delays.  Then for each slave with dc.enabled, configures
 * SYNC0 and/or SYNC1 signals.
 *
 * @param config Parsed EtherCAT configuration
 * @param logger Logger
 */
static void ecat_master_configure_dc(ecat_master_instance_t *inst, edog_logger_t *logger)
{
    const ecat_config_t *config = &inst->config;

    /* Step 1: Let SOEM discover DC-capable slaves and measure delays */
    edog_log_info(logger, "Configuring Distributed Clocks...");
    ecx_configdc(&inst->ecx_context);

    /* Step 2: Apply per-slave DC configuration */
    for (int i = 0; i < config->slave_count; i++) {
        const ecat_slave_t *slave = &config->slaves[i];
        int pos = slave->position;

        if (!slave->dc.enabled)
            continue;

        if (pos < 1 || pos > inst->ecx_context.slavecount) {
            edog_log_warn(logger,
                "Slave %d (%s): DC config skipped - position out of range",
                pos, slave->name);
            continue;
        }

        if (!inst->ecx_context.slavelist[pos].hasdc) {
            edog_log_warn(logger,
                "Slave %d (%s): DC config requested but slave has no DC support",
                pos, slave->name);
            continue;
        }

        /* Determine cycle time: use slave-specific or fall back to master cycle */
        uint32_t cycle_ns;
        if (slave->dc.sync_unit_cycle_us > 0) {
            cycle_ns = (uint32_t)(slave->dc.sync_unit_cycle_us * 1000);
        } else {
            cycle_ns = (uint32_t)(config->master.cycle_time_us * 1000);
        }

        if (slave->dc.sync0_enabled && slave->dc.sync1_enabled) {
            /* Both SYNC0 and SYNC1 */
            uint32_t cycle0_ns = (slave->dc.sync0_cycle_us > 0)
                ? (uint32_t)(slave->dc.sync0_cycle_us * 1000) : cycle_ns;
            uint32_t cycle1_ns = (slave->dc.sync1_cycle_us > 0)
                ? (uint32_t)(slave->dc.sync1_cycle_us * 1000) : cycle_ns;
            int32_t shift_ns = (int32_t)(slave->dc.sync0_shift_us * 1000);

            ecx_dcsync01(&inst->ecx_context, (uint16)pos, TRUE,
                         cycle0_ns, cycle1_ns, shift_ns);

            edog_log_info(logger,
                "Slave %d (%s): DC SYNC0+SYNC1 enabled "
                "(cycle0=%u ns, cycle1=%u ns, shift=%d ns)",
                pos, slave->name, cycle0_ns, cycle1_ns, shift_ns);

        } else if (slave->dc.sync0_enabled) {
            /* SYNC0 only */
            uint32_t sync0_ns = (slave->dc.sync0_cycle_us > 0)
                ? (uint32_t)(slave->dc.sync0_cycle_us * 1000) : cycle_ns;
            int32_t shift_ns = (int32_t)(slave->dc.sync0_shift_us * 1000);

            ecx_dcsync0(&inst->ecx_context, (uint16)pos, TRUE,
                        sync0_ns, shift_ns);

            edog_log_info(logger,
                "Slave %d (%s): DC SYNC0 enabled (cycle=%u ns, shift=%d ns)",
                pos, slave->name, sync0_ns, shift_ns);

        } else {
            /* DC enabled but no SYNC signals - just log it */
            edog_log_debug(logger,
                "Slave %d (%s): DC enabled but no SYNC signals configured",
                pos, slave->name);
        }
    }
}

/* Bits a slave maps per direction, from the same sources ecx_config_map_group reads. */
static void slave_mapped_bits(ecx_contextt *ctx, uint16 slave, uint32 *obits, uint32 *ibits)
{
    ec_slavet *sl = &ctx->slavelist[slave];
    uint32 osize = 0, isize = 0;
    if (sl->mbx_proto & ECT_MBXPROT_COE) {
        int ok = 0;
        if (sl->CoEdetails & ECT_COEDET_SDOCA)
            ok = ecx_readPDOmapCA(ctx, slave, 0, &osize, &isize);
        if (!ok) {
            osize = isize = 0;
            ecx_readPDOmap(ctx, slave, &osize, &isize);
        }
    }
    if (!osize && !isize && (sl->mbx_proto & ECT_MBXPROT_SOE))
        ecx_readIDNmap(ctx, slave, &osize, &isize);
    if (!osize && !isize) {
        ec_eepromPDOt pdo;
        memset(&pdo, 0, sizeof(pdo));
        isize = ecx_siiPDO(ctx, slave, &pdo, 0);
        memset(&pdo, 0, sizeof(pdo));
        osize = ecx_siiPDO(ctx, slave, &pdo, 1);
    }
    *obits = osize > sl->Obits ? osize : sl->Obits;
    *ibits = isize > sl->Ibits ? isize : sl->Ibits;
}

/* Grow the IOmap to what the slaves report after the startup SDOs, before SOEM maps into it. */
static int size_iomap_from_slaves(ecat_master_instance_t *inst, edog_logger_t *logger)
{
    ecx_contextt *ctx = &inst->ecx_context;
    size_t bytes = 0;
    for (int i = 1; i <= ctx->slavecount; i++) {
        uint32 obits = 0, ibits = 0;
        slave_mapped_bits(ctx, (uint16)i, &obits, &ibits);
        bytes += (obits + 7) / 8 + (ibits + 7) / 8 + ECAT_IOMAP_SLACK_PER_SLAVE;
    }
    if (bytes < ECAT_IOMAP_MIN_BYTES)
        bytes = ECAT_IOMAP_MIN_BYTES;
    if (ecat_master_instance_grow_iomap(inst, bytes) != 0) {
        edog_log_error(logger, "Master '%s': IOmap allocation of %zu bytes failed", inst->name,
                       bytes);
        return -1;
    }
    edog_log_info(logger, "Master '%s': IOmap %zu bytes (slaves report %zu with slack)",
                  inst->name, inst->iomap_capacity, bytes);
    return 0;
}

int ecat_master_configure(ecat_master_instance_t *inst, edog_logger_t *logger)
{
    const ecat_config_t *config = &inst->config;

    if (!inst->soem_initialized) {
        edog_log_error(logger, "Cannot configure: SOEM not initialized");
        return -1;
    }

    /* Step 4: map process data. A size of 0 means SOEM could not lay out the PDOs. */
    edog_log_info(logger, "Mapping process data...");

    if (size_iomap_from_slaves(inst, logger) != 0)
        return -1;

    int io_size = ecx_config_map_group(&inst->ecx_context, inst->iomap, 0);
    if (io_size <= 0) {
        edog_log_error(logger,
            "ecx_config_map_group returned %d -- process data mapping failed "
            "(likely SII / mailbox issue)", io_size);
        return -1;
    }
    if ((size_t)io_size > inst->iomap_capacity) {
        /* SOEM already wrote past the buffer's view of the mapping; it keeps the pointer, so
         * growing it now is unsafe. Sizing above makes this unreachable unless SOEM changes. */
        edog_log_error(logger,
            "IOmap mismatch: SOEM mapped %d bytes into a %zu-byte buffer", io_size,
            inst->iomap_capacity);
        return -1;
    }

    inst->iomap_used_size = (size_t)io_size;

    /* Cross-check: the per-group totals should add up to the same value
     * SOEM returned.  A mismatch is a SOEM bug or our config is racy --
     * not fatal but worth surfacing. */
    ec_groupt *grp = &inst->ecx_context.grouplist[0];
    uint32_t grp_total = (uint32_t)grp->Obytes + (uint32_t)grp->Ibytes;
    if ((int)grp_total != io_size) {
        edog_log_warn(logger,
            "IOmap size mismatch: ecx returned %d but grp totals=%u "
            "(Obytes=%d Ibytes=%d)",
            io_size, grp_total, grp->Obytes, grp->Ibytes);
    }

    edog_log_info(logger, "IO map: %d output bytes, %d input bytes, %d segments",
                       grp->Obytes, grp->Ibytes, grp->nsegments);

    /* Step 5: Configure watchdogs per slave.  Reuses slave->strict_sdo --
     * a slave that wants strict SDO writes also wants strict SM watchdog
     * writes; both are critical configuration the operator pinned in JSON. */
    edog_log_info(logger, "Configuring per-slave watchdogs...");
    for (int i = 0; i < config->slave_count; i++) {
        const ecat_slave_t *slave = &config->slaves[i];
        int pos = slave->position;
        if (pos < 1 || pos > inst->ecx_context.slavecount)
            continue;
        if (ecat_master_configure_watchdog(inst, pos, &slave->watchdog,
                                           slave->strict_sdo, logger) != 0) {
            edog_log_error(logger,
                "Master '%s': Slave %d (%s): SM watchdog config failed and "
                "strict_sdo=true -- aborting startup",
                inst->name, pos, slave->name);
            return -1;
        }
    }

    /* Step 6: Configure Distributed Clocks per slave */
    ecat_master_configure_dc(inst, logger);

    return 0;
}

/*
 * =============================================================================
 * Phase 4: Transition to OPERATIONAL
 * =============================================================================
 */

int ecat_master_transition_to_op(ecat_master_instance_t *inst, edog_logger_t *logger)
{
    const ecat_config_t *config = &inst->config;

    if (!inst->soem_initialized) {
        edog_log_error(logger, "Cannot transition: SOEM not initialized");
        return -1;
    }

    /* Compute maximum SAFE-OP->OP timeout across all configured slaves */
    int max_safeop_timeout_us = 0;
    for (int i = 0; i < config->slave_count; i++) {
        int t_us = config->slaves[i].timeouts.safeop_to_op_timeout_ms * 1000;
        if (t_us > max_safeop_timeout_us)
            max_safeop_timeout_us = t_us;
    }
    if (max_safeop_timeout_us == 0)
        max_safeop_timeout_us = EC_TIMEOUTSTATE * 4;

    /* Step 6: Wait for SAFE_OP after config */
    edog_log_info(logger, "Waiting for SAFE_OP state...");
    edog_log_debug(logger, "Using SAFE-OP->OP timeout: %d us", max_safeop_timeout_us);

    ecx_statecheck(&inst->ecx_context, 0, EC_STATE_SAFE_OP, max_safeop_timeout_us);

    /* Read back actual states */
    ecx_readstate(&inst->ecx_context);
    if (inst->ecx_context.slavelist[0].state != EC_STATE_SAFE_OP) {
        edog_log_error(logger,
            "Not all slaves reached SAFE_OP state (current state: 0x%04X)",
            inst->ecx_context.slavelist[0].state);

        /* Log individual slave states for debugging */
        for (int i = 1; i <= inst->ecx_context.slavecount; i++) {
            ec_slavet *slave = &inst->ecx_context.slavelist[i];
            if (slave->state != EC_STATE_SAFE_OP) {
                edog_log_error(logger,
                    "  Slave %d (%s): state=0x%04X, ALstatuscode=0x%04X",
                    i, slave->name, slave->state, slave->ALstatuscode);
            }
        }

        return -1;
    }

    edog_log_info(logger, "All slaves in SAFE_OP state");

    /* Step 7: Send initial process data and request OPERATIONAL */
    edog_log_info(logger, "Requesting OPERATIONAL state...");

    /* Send one round of process data to make slave outputs happy */
    ecx_send_processdata(&inst->ecx_context);
    ecx_receive_processdata(&inst->ecx_context, EC_TIMEOUTRET);

    /* Request OP state */
    inst->ecx_context.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&inst->ecx_context, 0);

    /* Poll for OP with exchanges in between; each wait is capped at a quarter of the smallest SM
     * watchdog so SM2 does not trip (AL 0x001B) during SAFE-OP. */
    int min_sm_wd_us = 0;
    for (int i = 0; i < config->slave_count; i++) {
        const ecat_watchdog_t *wd = &config->slaves[i].watchdog;
        if (wd->sm_watchdog_enabled && wd->sm_watchdog_ms > 0) {
            int wd_us = wd->sm_watchdog_ms * 1000;
            if (min_sm_wd_us == 0 || wd_us < min_sm_wd_us)
                min_sm_wd_us = wd_us;
        }
    }
    /* Default ESC SM watchdog is 100 ms.  Use it when no slave has an
     * explicit watchdog configured. */
    if (min_sm_wd_us == 0)
        min_sm_wd_us = 100 * 1000;

    int poll_timeout_us = min_sm_wd_us / 4;
    if (poll_timeout_us < EC_TIMEOUTRET)
        poll_timeout_us = EC_TIMEOUTRET;

    /* Total polling budget is the longest configured safeop->op timeout.
     * Derive the retry count from the cadence we need. */
    int retries = max_safeop_timeout_us / poll_timeout_us;
    if (retries < ECAT_OP_POLL_RETRIES)
        retries = ECAT_OP_POLL_RETRIES;

    edog_log_debug(logger,
        "OP poll cadence: timeout=%d us, retries=%d "
        "(min SM watchdog among slaves=%d us)",
        poll_timeout_us, retries, min_sm_wd_us);

    int op_reached = 0;
    for (int retry = 0; retry < retries; retry++) {
        ecx_send_processdata(&inst->ecx_context);
        ecx_receive_processdata(&inst->ecx_context, EC_TIMEOUTRET);
        ecx_statecheck(&inst->ecx_context, 0, EC_STATE_OPERATIONAL, poll_timeout_us);

        if (inst->ecx_context.slavelist[0].state == EC_STATE_OPERATIONAL) {
            op_reached = 1;
            break;
        }
    }

    if (!op_reached) {
        edog_log_error(logger,
            "Not all slaves reached OPERATIONAL state after %d retries "
            "(poll_timeout=%d us)",
            retries, poll_timeout_us);

        /* Log individual slave states for debugging */
        ecx_readstate(&inst->ecx_context);
        for (int i = 1; i <= inst->ecx_context.slavecount; i++) {
            ec_slavet *slave = &inst->ecx_context.slavelist[i];
            if (slave->state != EC_STATE_OPERATIONAL) {
                edog_log_error(logger,
                    "  Slave %d (%s): state=0x%04X, ALstatuscode=0x%04X",
                    i, slave->name, slave->state, slave->ALstatuscode);
            }
        }

        return -1;
    }

    edog_log_info(logger, "EtherCAT master operational with %d slave(s)",
                       inst->ecx_context.slavecount);

    return 0;
}

/*
 * =============================================================================
 * Master Close
 * =============================================================================
 */

void ecat_master_close(ecat_master_instance_t *inst, edog_logger_t *logger)
{
    if (inst->soem_initialized) {
        /* Step 1: zero the outputs before leaving OP, on every stop and exit; skipped before mapping */
        if (inst->iomap_used_size > 0) {
            edog_log_info(logger, "Zeroing outputs before close");
            memset(inst->iomap, 0, inst->iomap_used_size);
            int wkc = 0;
            for (int attempt = 0; attempt < ECAT_ZERO_OUTPUT_ATTEMPTS && wkc <= 0; attempt++) {
                ecx_send_processdata(&inst->ecx_context);
                wkc = ecx_receive_processdata(&inst->ecx_context, EC_TIMEOUTRET);
            }
            if (wkc <= 0) {
                edog_log_warn(logger,
                    "Zeroed outputs got no reply after %d attempts -- relying on the slaves' "
                    "SM watchdog", ECAT_ZERO_OUTPUT_ATTEMPTS);
            }
        }

        /* Step 2: request INIT and confirm the transition.  Discarding
         * wkc here meant slaves could remain in OP/SAFE-OP after stop_loop
         * if the broadcast did not reach them. */
        edog_log_info(logger, "Transitioning slaves to INIT state...");
        inst->ecx_context.slavelist[0].state = EC_STATE_INIT;
        int init_wkc = ecx_writestate(&inst->ecx_context, 0);
        if (init_wkc <= 0) {
            edog_log_error(logger,
                "writestate(INIT) returned wkc=%d -- slaves may remain in "
                "OP/SAFE-OP until their SM watchdog expires", init_wkc);
        } else {
            /* Short timeout -- not worth waiting safeop_to_op here. */
            ecx_statecheck(&inst->ecx_context, 0, EC_STATE_INIT, EC_TIMEOUTSTATE);
            ecx_readstate(&inst->ecx_context);
            int stuck = 0;
            for (int i = 1; i <= inst->ecx_context.slavecount; i++) {
                uint16_t st = inst->ecx_context.slavelist[i].state;
                if (st != EC_STATE_INIT) {
                    edog_log_warn(logger,
                        "Slave %d (%s): did not reach INIT (state=0x%04X) -- "
                        "fallback to slave SM watchdog",
                        i, inst->ecx_context.slavelist[i].name, st);
                    stuck++;
                }
            }
            if (stuck == 0) {
                edog_log_info(logger, "All slaves confirmed in INIT");
            }
        }

        /* Step 3: close the network interface */
        ecx_close(&inst->ecx_context);
        inst->soem_initialized = 0;
    }

    /* Always attempt to revert iface state, even if SOEM init had failed
     * after we already modified the interface.  Revert reads in-memory
     * flags and is a no-op when nothing was applied. */
    ecat_iface_state_revert(&inst->iface_state, logger);

    /* Release the IOmap and the snapshot; the next open_and_scan allocates both again. */
    if (inst->iomap != NULL && inst->iomap_capacity > 0) {
        memset(inst->iomap, 0, inst->iomap_capacity);
    }
    free(inst->iomap);
    inst->iomap = NULL;
    inst->iomap_capacity = 0;
    inst->iomap_used_size = 0;

    free(inst->slaves_snapshot);
    inst->slaves_snapshot = NULL;
    inst->slaves_snapshot_capacity = 0;
    inst->slaves_snapshot_count = 0;

    edog_log_info(logger, "EtherCAT master closed");
}

/*
 * =============================================================================
 * Process Data and State Access
 * =============================================================================
 */

int ecat_master_exchange_processdata(ecat_master_instance_t *inst, int timeout_us)
{
    ecx_send_processdata(&inst->ecx_context);
    int wkc = ecx_receive_processdata(&inst->ecx_context,
                                       (timeout_us > 0) ? timeout_us : EC_TIMEOUTRET);
    return wkc;
}

int ecat_master_get_expected_wkc(ecat_master_instance_t *inst)
{
    ec_groupt *grp = &inst->ecx_context.grouplist[0];
    return (grp->outputsWKC * 2) + grp->inputsWKC;
}

const ec_slavet *ecat_master_get_slave(ecat_master_instance_t *inst, int position)
{
    if (position < 1 || position > inst->ecx_context.slavecount)
        return NULL;
    return &inst->ecx_context.slavelist[position];
}

uint16_t ecat_master_get_slave_state(ecat_master_instance_t *inst, int position)
{
    if (position < 1 || position > inst->ecx_context.slavecount)
        return 0;
    return inst->ecx_context.slavelist[position].state;
}

/*
 * =============================================================================
 * Slave Recovery
 * =============================================================================
 */

/**
 * @brief Issue ecx_writestate during recovery, capturing wkc.
 *
 * wkc<=0 means the request did not reach the slave (link/cable issue,
 * not a config problem).  Tally these into recovery_writestate_failures
 * so the operator can distinguish "physical recovery failure" from
 * "slave reachable but rejecting state" in the diagnostics.
 *
 * @return ecx_writestate's wkc (positive on success, <=0 on no response)
 */
static int writestate_with_check(ecat_master_instance_t *inst, int position,
                                  uint16_t target_state, edog_logger_t *logger)
{
    inst->ecx_context.slavelist[position].state = target_state;
    int wkc = ecx_writestate(&inst->ecx_context, (uint16)position);
    if (wkc <= 0) {
        atomic_fetch_add_explicit(&inst->recovery_writestate_failures, 1,
                                  memory_order_relaxed);
        edog_log_warn(logger,
            "Slave %d (%s): writestate(0x%04X) wkc=%d -- request did not "
            "reach slave (link/cable issue?)",
            position, inst->ecx_context.slavelist[position].name,
            target_state, wkc);
    }
    return wkc;
}

int ecat_master_recover_slave(ecat_master_instance_t *inst, int position, edog_logger_t *logger)
{
    if (position < 1 || position > inst->ecx_context.slavecount) {
        edog_log_error(logger, "Invalid slave position %d for recovery", position);
        return -1;
    }

    ec_slavet *slave = &inst->ecx_context.slavelist[position];
    uint16_t current_state = slave->state;

    if (current_state == EC_STATE_OPERATIONAL) {
        /* Already operational */
        return 1;
    }

    if (current_state == (EC_STATE_SAFE_OP + EC_STATE_ERROR)) {
        /* SAFE_OP + ERROR: ACK the error, then request OP */
        edog_log_info(logger,
            "Slave %d (%s): SAFE_OP+ERROR (ALstatus=0x%04X), sending ACK",
            position, slave->name, slave->ALstatuscode);

        writestate_with_check(inst, position, EC_STATE_SAFE_OP + EC_STATE_ACK, logger);

        /* Now request OP */
        writestate_with_check(inst, position, EC_STATE_OPERATIONAL, logger);

        /* Check if it worked */
        ecx_statecheck(&inst->ecx_context, (uint16)position,
                        EC_STATE_OPERATIONAL, EC_TIMEOUTRET);

        if (slave->state == EC_STATE_OPERATIONAL) {
            edog_log_info(logger, "Slave %d (%s): recovered to OP",
                               position, slave->name);
            return 1;
        }
        return 0;
    }

    if (current_state == EC_STATE_SAFE_OP) {
        /* SAFE_OP: just request OP */
        edog_log_info(logger, "Slave %d (%s): in SAFE_OP, requesting OP",
                           position, slave->name);

        writestate_with_check(inst, position, EC_STATE_OPERATIONAL, logger);

        ecx_statecheck(&inst->ecx_context, (uint16)position,
                        EC_STATE_OPERATIONAL, EC_TIMEOUTRET);

        if (slave->state == EC_STATE_OPERATIONAL) {
            edog_log_info(logger, "Slave %d (%s): recovered to OP",
                               position, slave->name);
            return 1;
        }
        return 0;
    }

    if (current_state > EC_STATE_NONE) {
        /* Lower state but still present: try full reconfiguration */
        edog_log_info(logger,
            "Slave %d (%s): state=0x%04X, attempting reconfig",
            position, slave->name, current_state);

        if (ecx_reconfig_slave(&inst->ecx_context, (uint16)position, EC_TIMEOUTRET)) {
            slave->islost = FALSE;
            edog_log_info(logger, "Slave %d (%s): reconfigured", position, slave->name);

            /* After reconfig, check if it reached OP */
            ecx_statecheck(&inst->ecx_context, (uint16)position,
                            EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
            if (slave->state == EC_STATE_OPERATIONAL)
                return 1;
            return 0;
        }
        return 0;
    }

    /* EC_STATE_NONE: slave is lost, try recover */
    if (!slave->islost) {
        ecx_statecheck(&inst->ecx_context, (uint16)position,
                        EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
        if (slave->state == EC_STATE_NONE) {
            slave->islost = TRUE;
            edog_log_warn(logger, "Slave %d (%s): marked as lost",
                               position, slave->name);
        }
        return 0;
    }

    /* Slave was marked lost - try to recover */
    if (ecx_recover_slave(&inst->ecx_context, (uint16)position, EC_TIMEOUTRET)) {
        slave->islost = FALSE;
        edog_log_info(logger, "Slave %d (%s): recovered from lost state",
                           position, slave->name);
        return 1;
    }

    return 0;
}

void ecat_master_read_states(ecat_master_instance_t *inst)
{
    if (inst->soem_initialized)
        ecx_readstate(&inst->ecx_context);
}

void ecat_master_mark_all_operational(ecat_master_instance_t *inst)
{
    ecx_contextt *ctx = &inst->ecx_context;
    ctx->slavelist[0].state = EC_STATE_OPERATIONAL;
    ctx->slavelist[0].ALstatuscode = 0;
    for (int i = 1; i <= ctx->slavecount; i++) {
        ctx->slavelist[i].state = EC_STATE_OPERATIONAL;
        ctx->slavelist[i].ALstatuscode = 0;
    }
}

/* Defined by SOEM's Linux and Win32 nicdrv.c but not declared in their headers */
int ecx_inframe(ecx_portt *port, uint8 idx, int stacknumber);

int ecat_master_al_poll_send(ecat_master_instance_t *inst)
{
    ecx_portt *port = &inst->ecx_context.port;
    uint16 zero = 0;
    uint8 idx = ecx_getindex(port);
    ecx_setupdatagram(port, &(port->txbuf[idx]), EC_CMD_BRD, idx, 0, ECT_REG_ALSTAT, sizeof(zero),
                      &zero);
    if (ecx_outframe_red(port, idx) <= 0) {
        ecx_setbufstat(port, idx, EC_BUF_EMPTY);
        return -1;
    }
    return idx;
}

bool ecat_master_al_poll_collect(ecat_master_instance_t *inst, int idx, uint16_t *al_status,
                                 int *wkc)
{
    ecx_portt *port = &inst->ecx_context.port;
    /* Non-blocking: the reply stored by the process-data receive, or at most one packet read now */
    int rc = ecx_inframe(port, (uint8)idx, 0);
    bool got = rc >= 0;
    if (got) {
        uint16 le_status;
        memcpy(&le_status, &port->rxbuf[idx][EC_HEADERSIZE], sizeof(le_status));
        *al_status = etohs(le_status);
        *wkc = rc;
    }
    ecx_setbufstat(port, (uint8)idx, EC_BUF_EMPTY);
    return got;
}

/*
 * =============================================================================
 * IOmap Access
 * =============================================================================
 */

uint8_t *ecat_master_get_iomap(ecat_master_instance_t *inst)
{
    if (!inst->soem_initialized)
        return NULL;
    return inst->iomap;
}

size_t ecat_master_get_iomap_size(ecat_master_instance_t *inst)
{
    return inst->iomap_used_size;
}

int ecat_master_get_slave_count(ecat_master_instance_t *inst)
{
    if (!inst->soem_initialized)
        return 0;
    return inst->ecx_context.slavecount;
}

/*
 * =============================================================================
 * Modular couplers (ETG.5001): detected modules and diagnostics
 * =============================================================================
 */

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool slave_has_coe(const ecx_contextt *ctx, int slave_pos)
{
    return (ctx->slavelist[slave_pos].mbx_proto & ECT_MBXPROT_COE) != 0;
}

int ecat_read_module_idents(ecx_contextt *ctx, int slave_pos, uint32_t **idents_out,
                            int *count_out, char *err, size_t err_size, edog_logger_t *logger)
{
    *idents_out = NULL;
    *count_out = 0;
    if (slave_pos < 1 || slave_pos > ctx->slavecount) {
        snprintf(err, err_size, "slave position %d out of range (1..%d)", slave_pos,
                 ctx->slavecount);
        return -1;
    }
    if (!slave_has_coe(ctx, slave_pos)) {
        snprintf(err, err_size, "slave %d does not support CoE mailbox", slave_pos);
        return -1;
    }

    uint8_t subcount = 0;
    int size = (int)sizeof(subcount);
    int wkc = ecx_SDOread(ctx, (uint16)slave_pos, 0xF050, 0, FALSE, &size, &subcount,
                          EC_TIMEOUTRXM);
    if (wkc <= 0 || size != (int)sizeof(subcount)) {
        snprintf(err, err_size, "slave %d: 0xF050:0 read failed (wkc=%d); not a modular coupler?",
                 slave_pos, wkc);
        edog_log_warn(logger, "scan-modules slave=%d: 0xF050:0 read failed (wkc=%d)", slave_pos, wkc);
        return -1;
    }

    int n = (int)subcount;
    uint32_t *idents = calloc((size_t)(n > 0 ? n : 1), sizeof(uint32_t));
    if (idents == NULL) {
        snprintf(err, err_size, "out of memory");
        return -1;
    }
    for (int i = 1; i <= n; i++) {
        uint32_t ident = 0;
        size = (int)sizeof(ident);
        wkc = ecx_SDOread(ctx, (uint16)slave_pos, 0xF050, (uint8)i, FALSE, &size, &ident,
                          EC_TIMEOUTRXM);
        if (wkc <= 0 || size != (int)sizeof(ident)) {
            snprintf(err, err_size, "slave %d: 0xF050:%d read failed (wkc=%d, size=%d)", slave_pos,
                     i, wkc, size);
            edog_log_warn(logger, "scan-modules slave=%d: 0xF050:%d read failed (wkc=%d)",
                          slave_pos, i, wkc);
            free(idents);
            return -1;
        }
        idents[i - 1] = etohl(ident);
    }

    *idents_out = idents;
    *count_out = n;
    edog_log_info(logger, "scan-modules slave=%d: %d module ident(s) read", slave_pos, n);
    return 0;
}

int ecat_scan_modules_offline(const char *ifname, int slave_pos, int preop_timeout_us,
                              uint32_t **idents_out, int *count_out, char *err,
                              size_t err_size, edog_logger_t *logger)
{
    *idents_out = NULL;
    *count_out = 0;
    ecx_contextt *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        snprintf(err, err_size, "out of memory");
        return -1;
    }
    if (!ecx_init(ctx, ifname)) {
        ecat_soem_release_failed_init(ctx);
        snprintf(err, err_size, "failed to open interface '%s'", ifname);
        free(ctx);
        return -1;
    }

    int rc = -1;
    if (ecx_config_init(ctx) <= 0) {
        snprintf(err, err_size, "no EtherCAT slaves found on '%s'", ifname);
    } else if (slave_pos < 1 || slave_pos > ctx->slavecount) {
        snprintf(err, err_size, "slave position %d out of range (1..%d)", slave_pos,
                 ctx->slavecount);
    } else {
        /* The mailbox needs PRE-OP; ecx_config_init only requests it. */
        ecx_statecheck(ctx, (uint16)slave_pos, EC_STATE_PRE_OP,
                       preop_timeout_us > 0 ? preop_timeout_us : EC_TIMEOUTSTATE);
        if ((ctx->slavelist[slave_pos].state & 0x0F) < EC_STATE_PRE_OP) {
            snprintf(err, err_size, "slave %d did not reach PRE-OP (state=0x%04X)", slave_pos,
                     ctx->slavelist[slave_pos].state);
        } else {
            rc = ecat_read_module_idents(ctx, slave_pos, idents_out, count_out, err, err_size,
                                         logger);
        }
    }

    ctx->slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(ctx, 0);
    ecx_close(ctx);
    free(ctx);
    return rc;
}

/* Remaining budget in microseconds for one SDO, capped at the SOEM default. 0 when spent. */
static int budget_timeout_us(uint64_t deadline_ms)
{
    uint64_t now = monotonic_ms();
    if (now >= deadline_ms)
        return 0;
    uint64_t left_us = (deadline_ms - now) * 1000u;
    return left_us < (uint64_t)EC_TIMEOUTRXM ? (int)left_us : EC_TIMEOUTRXM;
}

/* Acyclic lock within the remaining budget; true when held and the bus is still up. */
static bool lock_for_sdo(ecat_master_instance_t *inst, uint64_t deadline_ms, int pos)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    uint64_t now = monotonic_ms();
    uint64_t wait_ms = deadline_ms > now ? deadline_ms - now : 0;
    until.tv_sec += (time_t)(wait_ms / 1000u);
    until.tv_nsec += (long)(wait_ms % 1000u) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_nsec -= 1000000000L;
        until.tv_sec += 1;
    }
    if (pthread_mutex_timedlock(&inst->acyclic_lock, &until) != 0)
        return false;
    if (!inst->soem_initialized || pos > inst->ecx_context.slavecount) {
        pthread_mutex_unlock(&inst->acyclic_lock);
        return false;
    }
    return true;
}

void ecat_coupler_diag_free(ecat_coupler_diag_t *d)
{
    if (d == NULL)
        return;
    free(d->modules);
    d->modules = NULL;
    d->module_count = 0;
}

int ecat_master_read_coupler_diagnostics(ecat_master_instance_t *inst, const ecat_slave_t *slave,
                                         int budget_ms, ecat_coupler_diag_t *out, char *err,
                                         size_t err_size, edog_logger_t *logger)
{
    memset(out, 0, sizeof(*out));
    ecx_contextt *ctx = &inst->ecx_context;
    int pos = slave->position;
    uint64_t deadline = monotonic_ms() + (uint64_t)(budget_ms > 0 ? budget_ms : 1);
    if (pos < 1 || !lock_for_sdo(inst, deadline, pos)) {
        snprintf(err, err_size, "the bus is not running or is recovering, retry");
        return -1;
    }
    bool has_coe = slave_has_coe(ctx, pos);
    pthread_mutex_unlock(&inst->acyclic_lock);
    if (!has_coe) {
        snprintf(err, err_size, "slave %d is not reachable over CoE", pos);
        return -1;
    }

    const ecat_modular_t *m = &slave->modular;
    out->modules = calloc((size_t)(m->module_count > 0 ? m->module_count : 1),
                          sizeof(ecat_module_diag_t));
    if (out->modules == NULL) {
        snprintf(err, err_size, "out of memory");
        return -1;
    }
    out->module_count = m->module_count;
    for (int i = 0; i < m->module_count; i++) {
        out->modules[i].slot = m->modules[i].slot;
        out->modules[i].ident = m->modules[i].ident;
    }

    /* 0xF100 by Complete Access: SI0 padded to 16 bits, then the status bits. */
    uint8_t buf[64];
    int size = (int)sizeof(buf);
    int wkc = 0;
    int t = 0;
    if (lock_for_sdo(inst, deadline, pos)) {
        t = budget_timeout_us(deadline);
        if (t > 0)
            wkc = ecx_SDOread(ctx, (uint16)pos, 0xF100, 0, TRUE, &size, buf, t);
        pthread_mutex_unlock(&inst->acyclic_lock);
    }
    if (wkc > 0 && size >= 4) {
        out->status_known = true;
        out->status_word = (uint16_t)(buf[2] | (buf[3] << 8));
    } else {
        edog_log_warn(logger, "diagnostics-slave slave=%d: 0xF100 read failed (wkc=%d, size=%d)",
                      pos, wkc, size);
    }

    int incr = m->slot_index_increment;
    for (int i = 0; i < out->module_count; i++) {
        ecat_module_diag_t *md = &out->modules[i];
        long index = 0xA000L + (long)(md->slot - 1) * incr;
        /* Without an increment every module would read slot 1's object. */
        if (incr <= 0 || index > 0xAFFF || !lock_for_sdo(inst, deadline, pos))
            continue;
        uint8_t state[4] = { 0 };
        size = (int)sizeof(state);
        wkc = 0;
        t = budget_timeout_us(deadline);
        if (t > 0)
            wkc = ecx_SDOread(ctx, (uint16)pos, (uint16)index, 1, FALSE, &size, state, t);
        pthread_mutex_unlock(&inst->acyclic_lock);
        if (wkc > 0 && size >= 1) {
            md->known = true;
            md->status = state[0];
        } else {
            edog_log_warn(logger, "diagnostics-slave slave=%d: 0x%04lX:1 read failed (wkc=%d)", pos,
                          index, wkc);
        }
    }
    return 0;
}

/*
 * =============================================================================
 * Fixed PDO mappings
 * =============================================================================
 */

static int verify_fixed_direction(ecat_master_instance_t *inst, const ecat_slave_t *slave,
                                  const ecat_pdo_t *pdos, int count, edog_logger_t *logger)
{
    ecx_contextt *ctx = &inst->ecx_context;
    int timeout = slave->timeouts.sdo_timeout_ms > 0 ? slave->timeouts.sdo_timeout_ms * 1000
                                                     : EC_TIMEOUTRXM;
    for (int p = 0; p < count; p++) {
        const ecat_pdo_t *pdo = &pdos[p];
        if (!pdo->fixed)
            continue;
        uint16_t index = (uint16_t)strtoul(pdo->index, NULL, 16);
        uint8_t n = 0;
        int size = (int)sizeof(n);
        int wkc = ecx_SDOread(ctx, (uint16)slave->position, index, 0, FALSE, &size, &n, timeout);
        if (wkc <= 0 || size != (int)sizeof(n)) {
            edog_log_warn(logger,
                "Slave %d (%s): mapping of fixed PDO 0x%04X not readable (wkc=%d); not verified",
                slave->position, slave->name, index, wkc);
            continue;
        }
        if (n != pdo->entry_count) {
            edog_log_error(logger,
                "Slave %d (%s): fixed PDO 0x%04X maps %u entries on the device, the configuration "
                "has %d",
                slave->position, slave->name, index, (unsigned)n, pdo->entry_count);
            return -1;
        }
        for (int e = 0; e < pdo->entry_count; e++) {
            const ecat_pdo_entry_t *entry = &pdo->entries[e];
            uint32_t expected = ((uint32_t)strtoul(entry->index, NULL, 16) << 16) |
                                ((uint32_t)entry->subindex << 8) | entry->bit_length;
            uint32_t actual = 0;
            size = (int)sizeof(actual);
            wkc = ecx_SDOread(ctx, (uint16)slave->position, index, (uint8)(e + 1), FALSE, &size,
                              &actual, timeout);
            if (wkc <= 0 || size != (int)sizeof(actual)) {
                edog_log_warn(logger,
                    "Slave %d (%s): 0x%04X:%d not readable (wkc=%d); fixed PDO not verified",
                    slave->position, slave->name, index, e + 1, wkc);
                break;
            }
            actual = etohl(actual);
            if (actual != expected) {
                edog_log_error(logger,
                    "Slave %d (%s): fixed PDO 0x%04X entry %d is 0x%08X on the device, the "
                    "configuration has 0x%08X",
                    slave->position, slave->name, index, e + 1, actual, expected);
                return -1;
            }
        }
    }
    return 0;
}

int ecat_master_verify_fixed_pdos(ecat_master_instance_t *inst, edog_logger_t *logger)
{
    for (int i = 0; i < inst->config.slave_count; i++) {
        const ecat_slave_t *slave = &inst->config.slaves[i];
        int pos = slave->position;
        /* Module PDOs live on the coupler's station, so only plain CoE slaves are checked. */
        if (slave->modular.present || pos < 1 || pos > inst->ecx_context.slavecount ||
            !slave_has_coe(&inst->ecx_context, pos))
            continue;
        if (verify_fixed_direction(inst, slave, slave->rx_pdos, slave->rx_pdo_count, logger) != 0 ||
            verify_fixed_direction(inst, slave, slave->tx_pdos, slave->tx_pdo_count, logger) != 0)
            return -1;
    }
    return 0;
}
