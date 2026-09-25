// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file bus.c
 * @brief EtherCAT masters: lifecycle, bus and monitor threads, and the JSON commands.
 *
 * Each "protocol":"ETHERCAT" entry of the bus configuration becomes an independent master
 * with its own SOEM context, IOmap, layout, data session, bus thread and monitor thread.
 *
 * Bus thread, per cycle (SCHED_FIFO, absolute clock_nanosleep to the next deadline):
 *   1. apply the newest output frame from the client into the IOmap (or the safe state)
 *   2. SOEM exchange
 *   3. send the input region to the client as one datagram
 *   4. collect the previous AL status poll and send the next one
 * The bus thread takes no lock the monitor holds across a round trip: SOEM serializes the socket
 * per frame, and the monitor owns slavelist[].
 */

/* _GNU_SOURCE: pthread_setname_np() for thread naming. Must precede system headers. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "bus.h"
#include "config.h"
#include "data.h"
#include "iface_state.h"
#include "layout.h"
#include "log.h"
#include "master.h"
#include "soem/soem.h"   /* osal_get_monotonic_time, ec_timet */
#include "cjson/cJSON.h"

/* Forward declaration: ecat_bus_thread is defined alongside the bus
 * loop further down in the file but referenced first by
 * start_single_master via pthread_create. */
static void *ecat_bus_thread(void *arg);
static bool master_has_layout(ecat_master_instance_t *inst);

/*
 * =============================================================================
 * Constants
 * =============================================================================
 */

/** Minimum timeout for receive in microseconds */
#define ECAT_MIN_RECEIVE_TIMEOUT_US 200

/*
 * =============================================================================
 * Inline Timing Helpers
 * =============================================================================
 */

/**
 * @brief Convert ec_timet (struct timespec) to nanoseconds
 */
static inline uint64_t timespec_to_ns(const ec_timet *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

/**
 * @brief Return elapsed nanoseconds between two timestamps
 */
static inline uint64_t elapsed_ns(const ec_timet *start, const ec_timet *end)
{
    uint64_t s = timespec_to_ns(start);
    uint64_t e = timespec_to_ns(end);
    return (e > s) ? (e - s) : 0;
}

/*
 * =============================================================================
 * EtherCAT AL State to String Helper
 * =============================================================================
 */

/**
 * @brief Convert SOEM EC_STATE_* value to a human-readable string
 */
static const char *al_state_to_string(uint16_t state)
{
    /* Mask off the error bit for comparison */
    uint16_t base = state & 0x0F;
    int has_error = (state & EC_STATE_ERROR) != 0;

    const char *name;
    switch (base) {
    case EC_STATE_NONE:     name = "NONE";     break;
    case EC_STATE_INIT:     name = "INIT";     break;
    case EC_STATE_PRE_OP:   name = "PRE-OP";   break;
    case EC_STATE_BOOT:     name = "BOOT";     break;
    case EC_STATE_SAFE_OP:  name = "SAFE-OP";  break;
    case EC_STATE_OPERATIONAL: name = "OP";     break;
    default:                name = "UNKNOWN";  break;
    }

    /* Return static strings for common cases */
    if (!has_error)
        return name;

    /* For error states, we just note it. Since we return static strings,
     * use a small set of pre-defined error state strings. */
    switch (base) {
    case EC_STATE_INIT:    return "INIT+ERR";
    case EC_STATE_PRE_OP:  return "PRE-OP+ERR";
    case EC_STATE_SAFE_OP: return "SAFE-OP+ERR";
    default:               return "UNKNOWN+ERR";
    }
}

/* Called before the bus and monitor threads exist, so memset is safe. Min sentinels start at the
 * maximum so the first sample wins. */
static void diag_reset(ecat_cycle_diag_t *d)
{
    memset(d, 0, sizeof(*d));
    atomic_store_explicit(&d->min_bus_cycle_ns, UINT64_MAX, memory_order_relaxed);
    atomic_store_explicit(&d->min_period_ns,    UINT64_MAX, memory_order_relaxed);
    atomic_store_explicit(&d->min_latency_ns,   INT64_MAX,  memory_order_relaxed);
}

/* PRIO_INHERIT so the SCHED_FIFO bus thread is not stuck behind a lower-priority holder. */
static int ecat_mutex_init_pi(pthread_mutex_t *m)
{
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0)
        return -1;
#if !defined(__CYGWIN__) && !defined(_WIN32)
    /* Best-effort: ignore failure (some libcs return ENOTSUP) */
    (void)pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
#endif
    int rc = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    return rc;
}


/* g_masters is replaced as a whole by configure, only while every master is stopped. */

static edog_logger_t g_logger;
static ecat_master_instance_t *g_masters = NULL;  /* heap-allocated array */
static int g_master_count = 0;
static char g_config_path[512] = "";
static char g_state_dir[256] = EDOG_DEFAULT_STATE_DIR;

/*
 * =============================================================================
 * Per-Instance Slaves Snapshot
 * =============================================================================
 */

/**
 * @brief Build and publish a snapshot of per-slave AL state.
 *
 * Copies slavelist[] states into the snapshot under slaves_mutex. Called by the monitor thread,
 * or before it exists. Counters are not cached here; queries read them from inst->diag.
 */
static void publish_slaves_snapshot(ecat_master_instance_t *inst)
{
    ecat_slave_status_t local[ECAT_MAX_SLAVES];
    memset(local, 0, sizeof(local));

    int n = inst->config.slave_count;
    if (n > ECAT_MAX_SLAVES)
        n = ECAT_MAX_SLAVES;

    for (int i = 0; i < n; i++) {
        const ecat_slave_t *cfg = &inst->config.slaves[i];
        ecat_slave_status_t *ss = &local[i];

        ss->position = cfg->position;
        strncpy(ss->name, cfg->name, ECAT_MAX_NAME_LEN - 1);
        ss->name[ECAT_MAX_NAME_LEN - 1] = '\0';

        const ec_slavet *soem = ecat_master_get_slave(inst, cfg->position);
        if (soem) {
            ss->al_state = soem->state;
            ss->al_status_code = soem->ALstatuscode;
        }
    }

    pthread_mutex_lock(&inst->slaves_mutex);
    memcpy(inst->slaves_snapshot, local, sizeof(local));
    inst->slaves_snapshot_count = n;
    pthread_mutex_unlock(&inst->slaves_mutex);
}

#if ECAT_ENABLE_MONITOR_THREAD

/** Map ec_err_type to a short human-readable tag for log lines. */
static const char *ecat_err_type_name(ec_err_type t)
{
    switch (t) {
    case EC_ERR_TYPE_SDO_ERROR:           return "SDO";
    case EC_ERR_TYPE_EMERGENCY:           return "EMERGENCY";
    case EC_ERR_TYPE_PACKET_ERROR:        return "PACKET";
    case EC_ERR_TYPE_SDOINFO_ERROR:       return "SDOINFO";
    case EC_ERR_TYPE_FOE_ERROR:           return "FOE";
    case EC_ERR_TYPE_FOE_BUF2SMALL:       return "FOE_BUF2SMALL";
    case EC_ERR_TYPE_FOE_PACKETNUMBER:    return "FOE_PKTNUM";
    case EC_ERR_TYPE_SOE_ERROR:           return "SOE";
    case EC_ERR_TYPE_MBX_ERROR:           return "MBX";
    case EC_ERR_TYPE_FOE_FILE_NOTFOUND:   return "FOE_NOTFOUND";
    case EC_ERR_TYPE_EOE_INVALID_RX_DATA: return "EOE_RX";
    }
    return "UNKNOWN";
}

/**
 * @brief Log one popped ec_errort.
 *
 * Emergency Error Code 0x0000 (CiA 301) is a "no error / error reset"
 * notification -- log at info level so operators can see drives clearing
 * faults, but don't escalate to warn/error.
 */
static void log_ecat_error(const ecat_master_instance_t *inst, const ec_errort *e)
{
    const char *type = ecat_err_type_name(e->Etype);

    if (e->Etype == EC_ERR_TYPE_EMERGENCY) {
        /* CiA 301 "Error reset / no error" carries ErrorCode 0x0000.
         * Surface as info so operators can see drives clearing faults
         * without escalating to error level. */
        if (e->ErrorCode == 0x0000) {
            edog_log_info(&g_logger,
                "Master '%s': %s slave=%u code=0x%04X reg=0x%02X "
                "data=%02X %04X %04X (error reset)",
                inst->name, type, (unsigned)e->Slave,
                (unsigned)e->ErrorCode, (unsigned)e->ErrorReg,
                (unsigned)e->b1, (unsigned)e->w1, (unsigned)e->w2);
        } else {
            edog_log_error(&g_logger,
                "Master '%s': %s slave=%u code=0x%04X reg=0x%02X "
                "data=%02X %04X %04X",
                inst->name, type, (unsigned)e->Slave,
                (unsigned)e->ErrorCode, (unsigned)e->ErrorReg,
                (unsigned)e->b1, (unsigned)e->w1, (unsigned)e->w2);
        }
    } else {
        edog_log_warn(&g_logger,
            "Master '%s': %s slave=%u index=0x%04X:%u abort=0x%08X",
            inst->name, type, (unsigned)e->Slave,
            (unsigned)e->Index, (unsigned)e->SubIdx,
            (unsigned)e->AbortCode);
    }
}

/**
 * @brief Drain SM1 mailboxes and SOEM's internal error queue.
 *
 * Runs ecx_mbxhandler (CoE emergencies land in SOEM's error list), then pops and logs the errors.
 * Monitor thread only; no-op when SOEM is not initialized.
 */
static void drain_mailbox_and_errors(ecat_master_instance_t *inst)
{
    if (!inst->soem_initialized)
        return;

    /* EC_MAXELIST is SOEM's queue capacity; we never need more slots. */
    ec_errort errors[EC_MAXELIST];
    int err_count = 0;

    /* At most 8 mailboxes per pass */
    ecx_mbxhandler(&inst->ecx_context, 0, 8);
    while (err_count < EC_MAXELIST &&
           ecx_poperror(&inst->ecx_context, &errors[err_count])) {
        err_count++;
    }

    for (int i = 0; i < err_count; i++)
        log_ecat_error(inst, &errors[i]);
}

/*
 * =============================================================================
 * Recovery (monitor thread only)
 * =============================================================================
 */

/**
 * @brief Attempt to recover all slaves that are not in OP
 *
 * Runs beside the bus thread, which keeps exchanging with the healthy slaves.
 *
 * @param inst Per-master instance
 * @return 1 if all slaves back in OP, 0 if some still recovering, -1 on max attempts
 */
static int attempt_recovery(ecat_master_instance_t *inst)
{
    ecat_master_read_states(inst);

    int all_ok = 1;

    for (int i = 0; i < inst->config.slave_count; i++) {
        int pos = inst->config.slaves[i].position;

        uint16_t state = ecat_master_get_slave_state(inst, pos);
        /* 0: already in OP; negative: recovery error */
        int result = 0;
        if (state != EC_STATE_OPERATIONAL) {
            all_ok = 0;
            result = ecat_master_recover_slave(inst, pos, &g_logger);
        }

        if (result < 0) {
            edog_log_error(&g_logger,
                "Master '%s': Slave %d (%s): recovery error",
                inst->name, pos, inst->config.slaves[i].name);
        }
    }

    if (all_ok) {
        edog_log_info(&g_logger,
            "Master '%s': All slaves recovered to OPERATIONAL (attempts=%d)",
            inst->name, atomic_load(&inst->recovery_attempts));
        atomic_store(&inst->recovery_attempts, 0);
        atomic_store(&inst->recovery_writestate_failures, 0);
        atomic_store(&inst->consecutive_wkc_errors, 0);
        return 1;
    }

    /* Single writer (monitor thread) */
    int attempts = atomic_load_explicit(&inst->recovery_attempts,
                                        memory_order_relaxed) + 1;
    atomic_store_explicit(&inst->recovery_attempts, attempts,
                          memory_order_relaxed);

    if (attempts >= ECAT_MAX_RECOVERY_ATTEMPTS) {
        edog_log_error(&g_logger,
            "Master '%s': Maximum recovery attempts (%d) reached - transitioning to ERROR",
            inst->name, ECAT_MAX_RECOVERY_ATTEMPTS);
        return -1;
    }

    edog_log_warn(&g_logger,
        "Master '%s': Recovery attempt %d/%d - some slaves not yet in OP",
        inst->name, attempts, ECAT_MAX_RECOVERY_ATTEMPTS);
    return 0;
}

/*
 * =============================================================================
 * Background Monitor Thread (per-instance)
 * =============================================================================
 */

/**
 * @brief Background thread for slave state monitoring, recovery, and logging.
 *
 * Runs at default (non-RT) priority. Periodically:
 *   - refreshes slave states: from the cyclic AL poll while it shows all slaves in OP,
 *     otherwise with ecx_readstate(); then publishes the slaves snapshot
 *   - performs recovery in RECOVERING
 *   - drains mailboxes
 *   - logs state and counter transitions, so the bus thread never logs
 * Its frames share the socket with the bus thread per frame; it never makes the bus thread wait.
 *
 * @param arg Pointer to the ecat_master_instance_t for this master
 */
static void *ecat_monitor_thread(void *arg)
{
    ecat_master_instance_t *inst = (ecat_master_instance_t *)arg;

    /* Log on transitions only; continuous metrics are served by status and diagnostics. */
    int last_logged_state = -1;
    int last_logged_consec_wkc = 0;
    uint64_t last_al_replies = 0;
    uint64_t last_al_faults = atomic_load(&inst->al_faults);

    edog_log_info(&g_logger,
        "Master '%s': monitor thread started (interval=%d ms)",
        inst->name, ECAT_MONITOR_INTERVAL_MS);

    while (atomic_load(&inst->monitor_running)) {
        int state = atomic_load(&inst->bus_state);

        if (state == ECAT_STATE_OPERATIONAL) {
            /* New replies and no faults since the last pass: all slaves in OP, no frame needed */
            uint64_t replies = atomic_load(&inst->al_replies);
            uint64_t faults = atomic_load(&inst->al_faults);
            if (replies != last_al_replies && faults == last_al_faults)
                ecat_master_mark_all_operational(inst);
            else
                ecat_master_read_states(inst);
            last_al_replies = replies;
            last_al_faults = faults;
            publish_slaves_snapshot(inst);

        } else if (state == ECAT_STATE_RECOVERING) {
            int result = attempt_recovery(inst);
            publish_slaves_snapshot(inst);

            if (result == 1) {
                atomic_store(&inst->bus_state, ECAT_STATE_OPERATIONAL);
                edog_log_info(&g_logger,
                    "Master '%s': [state: OPERATIONAL] Recovered from error",
                    inst->name);
            } else if (result == -1) {
                atomic_store(&inst->bus_state, ECAT_STATE_ERROR);
                edog_log_error(&g_logger,
                    "Master '%s': entered ERROR state after max recovery attempts",
                    inst->name);
            }
        }

        /* Mailboxes only while slaves are at least PRE-OP */
        int mbx_state = atomic_load(&inst->bus_state);
        if (mbx_state == ECAT_STATE_OPERATIONAL ||
            mbx_state == ECAT_STATE_RECOVERING) {
            drain_mailbox_and_errors(inst);
        }

        /* --- Transition-based logging --- */

        int curr_state = atomic_load(&inst->bus_state);
        if (curr_state != last_logged_state) {
            if (curr_state == ECAT_STATE_RECOVERING) {
                uint32_t al_trigger = atomic_load(&inst->recovery_al_trigger);
                if (al_trigger == 0)
                    edog_log_warn(&g_logger,
                        "Master '%s': WKC error threshold (%d) reached, "
                        "[state: RECOVERING]",
                        inst->name, ECAT_WKC_ERROR_THRESHOLD);
                else
                    edog_log_warn(&g_logger,
                        "Master '%s': AL status 0x%04X: not all slaves in OP, "
                        "[state: RECOVERING]",
                        inst->name, (unsigned)(al_trigger & 0xFFFF));
            }
            last_logged_state = curr_state;
        }

        int consec = atomic_load(&inst->consecutive_wkc_errors);
        if (consec > 0 && last_logged_consec_wkc == 0) {
            edog_log_warn(&g_logger,
                "Master '%s': WKC errors detected (consecutive=%d, expected=%d)",
                inst->name, consec, inst->expected_wkc);
            last_logged_consec_wkc = consec;
        } else if (consec == 0 && last_logged_consec_wkc != 0) {
            edog_log_info(&g_logger,
                "Master '%s': WKC errors cleared", inst->name);
            last_logged_consec_wkc = 0;
        }

        /* Sleep for the monitor interval */
        struct timespec sleep_ts;
        sleep_ts.tv_sec = ECAT_MONITOR_INTERVAL_MS / 1000;
        sleep_ts.tv_nsec = (ECAT_MONITOR_INTERVAL_MS % 1000) * 1000000L;
        nanosleep(&sleep_ts, NULL);
    }

    edog_log_info(&g_logger, "Master '%s': monitor thread exiting", inst->name);
    return NULL;
}
#endif /* ECAT_ENABLE_MONITOR_THREAD */

/*
 * =============================================================================
 * Per-Instance Helpers for start_loop / stop_loop / cycle_start
 * =============================================================================
 */

/**
 * @brief Start a single EtherCAT master instance
 *
 * Runs through SCANNING -> CONFIGURING -> TRANSITIONING -> OPERATIONAL
 * for one master. On failure the instance is set to ERROR state.
 *
 * @param inst Per-master instance
 * @return 0 on success, -1 on failure
 */
static int start_single_master(ecat_master_instance_t *inst)
{
    if (inst->config.slave_count == 0) {
        edog_log_warn(&g_logger,
            "Master '%s': No slaves configured - skipping", inst->name);
        return -1;
    }

    /* --- Phase 1: SCANNING --- */
    atomic_store(&inst->bus_state, ECAT_STATE_SCANNING);
    edog_log_info(&g_logger,
        "Master '%s': [state: SCANNING] Opening interface and scanning bus...",
        inst->name);

    if (ecat_master_open_and_scan(inst, &g_logger) != 0) {
        edog_log_error(&g_logger, "Master '%s': Bus scan failed", inst->name);
        /* open_and_scan may have partially applied iface state -- close reverts it. */
        ecat_master_close(inst, &g_logger);
        atomic_store(&inst->bus_state, ECAT_STATE_ERROR);
        return -1;
    }

    /* --- Phase 2: CONFIGURING (SDO writes + PDO mapping) --- */
    atomic_store(&inst->bus_state, ECAT_STATE_CONFIGURING);
    edog_log_info(&g_logger,
        "Master '%s': [state: CONFIGURING] Writing SDOs and mapping process data...",
        inst->name);

    /* Write SDOs for each slave that has them configured.  When
     * slave->strict_sdo is true (default) any failed write aborts the
     * master so it never enters OPERATIONAL with a half-configured slave. */
    for (int i = 0; i < inst->config.slave_count; i++) {
        const ecat_slave_t *slave = &inst->config.slaves[i];
        if (slave->sdo_count == 0)
            continue;

        int rc = ecat_master_write_sdos(inst, slave->position, slave->sdo_configs,
                                        slave->sdo_count,
                                        slave->timeouts.sdo_timeout_ms, &g_logger);
        if (rc != 0 && slave->strict_sdo) {
            edog_log_error(&g_logger,
                "Master '%s': Slave %d (%s): SDO config failed and strict_sdo=true -- aborting startup",
                inst->name, slave->position, slave->name);
            ecat_master_close(inst, &g_logger);
            atomic_store(&inst->bus_state, ECAT_STATE_ERROR);
            return -1;
        }
    }

    /* Map process data and configure DC */
    if (ecat_master_configure(inst, &g_logger) != 0) {
        edog_log_error(&g_logger,
            "Master '%s': Process data mapping failed", inst->name);
        ecat_master_close(inst, &g_logger);
        atomic_store(&inst->bus_state, ECAT_STATE_ERROR);
        return -1;
    }

    /* Publish where every configured PDO entry sits in the exchanged image. */
    if (ecat_layout_build(inst, &g_logger) != 0) {
        edog_log_error(&g_logger,
            "Master '%s': process data layout failed -- aborting startup",
            inst->name);
        ecat_master_close(inst, &g_logger);
        atomic_store(&inst->bus_state, ECAT_STATE_ERROR);
        return -1;
    }

    /* Cache expected WKC */
    inst->expected_wkc = ecat_master_get_expected_wkc(inst);
    edog_log_info(&g_logger, "Master '%s': Expected WKC: %d",
                       inst->name, inst->expected_wkc);

    inst->receive_timeout_us = inst->config.master.receive_timeout_us;
    if (inst->receive_timeout_us < ECAT_MIN_RECEIVE_TIMEOUT_US)
        inst->receive_timeout_us = ECAT_MIN_RECEIVE_TIMEOUT_US;
    edog_log_info(&g_logger,
        "Master '%s': Receive timeout: %d us (cycle_time=%d us)",
        inst->name, inst->receive_timeout_us, inst->config.master.cycle_time_us);

    /* --- Phase 3: TRANSITIONING --- */
    atomic_store(&inst->bus_state, ECAT_STATE_TRANSITIONING);
    edog_log_info(&g_logger,
        "Master '%s': [state: TRANSITIONING] Moving slaves to OPERATIONAL...",
        inst->name);

    if (ecat_master_transition_to_op(inst, &g_logger) != 0) {
        edog_log_error(&g_logger,
            "Master '%s': Failed to reach OPERATIONAL state", inst->name);
        ecat_master_close(inst, &g_logger);
        atomic_store(&inst->bus_state, ECAT_STATE_ERROR);
        return -1;
    }

    /* --- Phase 4: OPERATIONAL --- */
    atomic_store(&inst->consecutive_wkc_errors, 0);
    atomic_store(&inst->recovery_attempts, 0);
    atomic_store(&inst->recovery_writestate_failures, 0);
    inst->cycle_counter = 0;
    diag_reset(&inst->diag);

    /* EWMA window spanning ECAT_AVG_TARGET_WINDOW_NS at this cycle time */
    {
        int64_t cycle_ns = (int64_t)inst->config.master.cycle_time_us * 1000LL;
        inst->avg_window = (cycle_ns > 0)
            ? (ECAT_AVG_TARGET_WINDOW_NS / cycle_ns)
            : 1;
        if (inst->avg_window < 1) inst->avg_window = 1;
    }

    atomic_store(&inst->overruns, 0);
    inst->al_poll_idx = -1;
    inst->consecutive_al_faults = 0;
    atomic_store(&inst->al_status, 0);
    atomic_store(&inst->al_wkc, 0);
    atomic_store(&inst->al_replies, 0);
    atomic_store(&inst->al_misses, 0);
    atomic_store(&inst->al_faults, 0);
    atomic_store(&inst->recovery_al_trigger, 0);

    atomic_store(&inst->bus_state, ECAT_STATE_OPERATIONAL);

    /* Send one initial exchange to populate the IOmap with slave data */
    ecat_master_exchange_processdata(inst, inst->receive_timeout_us);

    /* Initial publish before the monitor and bus threads exist: no concurrency yet. */
    publish_slaves_snapshot(inst);

#if ECAT_ENABLE_MONITOR_THREAD
    atomic_store(&inst->monitor_running, true);

    if (pthread_create(&inst->monitor_thread, NULL, ecat_monitor_thread, inst) != 0) {
        edog_log_warn(&g_logger,
            "Master '%s': Failed to create monitor thread - "
            "running without state monitoring", inst->name);
        atomic_store(&inst->monitor_running, false);
    }
#endif

    /* SCHED_FIFO, absolute clock_nanosleep at master.cycle_time_us */
    atomic_store(&inst->bus_running, true);
    if (pthread_create(&inst->bus_thread, NULL, ecat_bus_thread, inst) != 0) {
        edog_log_error(&g_logger,
            "Master '%s': Failed to create bus thread: %s",
            inst->name, strerror(errno));
        atomic_store(&inst->bus_running, false);
        atomic_store(&inst->bus_state, ECAT_STATE_ERROR);
        return -1;
    }

    edog_log_info(&g_logger,
        "Master '%s': [state: OPERATIONAL] EtherCAT master started "
        "(dedicated bus thread, cycle=%d us, monitor=%s)",
        inst->name, inst->config.master.cycle_time_us,
        ECAT_ENABLE_MONITOR_THREAD ? "enabled" : "disabled");

    return 0;
}

/**
 * @brief Stop a single master instance
 *
 * Stops the monitor thread, logs final diagnostics, and closes the master.
 *
 * @param inst Per-master instance
 */
static void stop_single_master(ecat_master_instance_t *inst)
{
    int state = atomic_load(&inst->bus_state);
    if (state == ECAT_STATE_STOPPED || state == ECAT_STATE_IDLE) {
        edog_log_debug(&g_logger,
            "Master '%s': already stopped/idle", inst->name);
        return;
    }

    edog_log_info(&g_logger,
        "Master '%s': Stopping (current state: %s)...",
        inst->name, ecat_state_to_string(state));

    /* Bus thread first; SIGUSR1 wakes its clock_nanosleep */
    if (atomic_load(&inst->bus_running)) {
        atomic_store(&inst->bus_running, false);
        pthread_kill(inst->bus_thread, SIGUSR1);
        pthread_join(inst->bus_thread, NULL);
        edog_log_debug(&g_logger,
            "Master '%s': Bus thread joined", inst->name);
    }

#if ECAT_ENABLE_MONITOR_THREAD
    /* Stop the monitor thread before closing the master */
    if (atomic_load(&inst->monitor_running)) {
        atomic_store(&inst->monitor_running, false);
        pthread_join(inst->monitor_thread, NULL);
        edog_log_debug(&g_logger,
            "Master '%s': Monitor thread joined", inst->name);
    }
#endif

    /* Final diagnostics via relaxed atomic loads; cross-field tearing is harmless here. */
    uint64_t final_cycle_count =
        atomic_load_explicit(&inst->diag.cycle_count, memory_order_relaxed);
    uint64_t final_wkc_errors =
        atomic_load_explicit(&inst->diag.wkc_error_count, memory_order_relaxed);
    int64_t  final_sum =
        atomic_load_explicit(&inst->diag.avg_bus_cycle_ns_sum, memory_order_relaxed);
    uint64_t final_min =
        atomic_load_explicit(&inst->diag.min_bus_cycle_ns, memory_order_relaxed);
    uint64_t final_max =
        atomic_load_explicit(&inst->diag.max_bus_cycle_ns, memory_order_relaxed);

    if (final_cycle_count > 0 || final_wkc_errors > 0) {
        uint64_t total_cycles = final_cycle_count + final_wkc_errors;
        int64_t  final_avg_ns = (inst->avg_window > 0)
            ? final_sum / inst->avg_window
            : 0;
        /* min sentinel UINT64_MAX -> "n/a" */
        unsigned long long min_us = (final_min == UINT64_MAX) ? 0
                                  : (unsigned long long)(final_min / 1000);
        edog_log_info(&g_logger,
            "Master '%s': Final cycle stats: %llu total (%llu ok, %llu errors), "
            "avg=%lld us, min=%llu us, max=%llu us",
            inst->name,
            (unsigned long long)total_cycles,
            (unsigned long long)final_cycle_count,
            (unsigned long long)final_wkc_errors,
            (long long)(final_avg_ns / 1000),
            min_us,
            (unsigned long long)(final_max / 1000));
    }

    /* IP-stack isolation revert happens inside ecat_master_close(). */

    /* The layout dies with the mapping; a client must reopen its session after a restart. */
    ecat_data_close(inst);
    memset(&inst->layout, 0, sizeof(inst->layout));
    atomic_store(&inst->consecutive_wkc_errors, 0);
    atomic_store(&inst->recovery_attempts, 0);
    atomic_store(&inst->recovery_writestate_failures, 0);
    inst->expected_wkc = 0;

    ecat_master_close(inst, &g_logger);
    atomic_store(&inst->bus_state, ECAT_STATE_STOPPED);

    /* Bus closed: the old AL states mean nothing */
    pthread_mutex_lock(&inst->slaves_mutex);
    memset(inst->slaves_snapshot, 0, sizeof(inst->slaves_snapshot));
    inst->slaves_snapshot_count = 0;
    pthread_mutex_unlock(&inst->slaves_mutex);

    edog_log_info(&g_logger,
        "Master '%s': [state: STOPPED] EtherCAT master stopped", inst->name);
}

/**
 * @brief Perform one EtherCAT cycle for a single master instance
 *
 * Called by the dedicated bus thread on every cycle:
 *
 *   1. apply the newest output frame from the client (or the safe state) into the IOmap
 *   2. SOEM exchange
 *   3. send the input region to the client
 *   4. collect the previous AL status poll, send the next one
 *   5. update WKC + diagnostics
 *
 * Only this thread touches the IOmap. It never waits on the monitor: SOEM locks the socket per
 * frame, with priority inheritance on Linux.
 *
 * Returns true on an exchange, false when the master is not exchanging.
 */
static bool ecat_run_one_cycle(ecat_master_instance_t *inst)
{
    int state = atomic_load(&inst->bus_state);
    /* RECOVERING keeps exchanging while the monitor recovers slaves */
    if (state != ECAT_STATE_OPERATIONAL && state != ECAT_STATE_RECOVERING)
        return false;

    uint8_t *iomap = ecat_master_get_iomap(inst);
    if (!iomap)
        return false;

    ec_timet t_exch_start, t_exch_end;

    int master_index = (int)(inst - g_masters);

    /* 1. Newest outputs from the client, or the safe state if it went quiet. */
    ecat_data_apply_outputs(inst, master_index);

    /* 2. Exchange process data with slaves. */
    osal_get_monotonic_time(&t_exch_start);
    int wkc = ecat_master_exchange_processdata(inst, inst->receive_timeout_us);
    osal_get_monotonic_time(&t_exch_end);

    uint64_t exchange_ns = elapsed_ns(&t_exch_start, &t_exch_end);

    /* 3. Inputs to the client, flagged with bus health for this cycle. */
    ecat_data_publish_inputs(inst, master_index, wkc, state == ECAT_STATE_OPERATIONAL,
                             wkc >= inst->expected_wkc);

    /* 4. AL status: the previous reply came in with this cycle's frames */
    if (inst->al_poll_idx >= 0) {
        uint16_t al_status = 0;
        int al_wkc = 0;
        if (ecat_master_al_poll_collect(inst, inst->al_poll_idx, &al_status, &al_wkc)) {
            atomic_store_explicit(&inst->al_status, al_status, memory_order_relaxed);
            atomic_store_explicit(&inst->al_wkc, al_wkc, memory_order_relaxed);
            if (ecat_al_poll_healthy(al_status, al_wkc, inst->ecx_context.slavecount)) {
                inst->consecutive_al_faults = 0;
            } else {
                inst->consecutive_al_faults++;
                atomic_fetch_add_explicit(&inst->al_faults, 1, memory_order_relaxed);
            }
            /* Counted after faults: the monitor reads replies first */
            atomic_fetch_add_explicit(&inst->al_replies, 1, memory_order_release);
        } else {
            atomic_fetch_add_explicit(&inst->al_misses, 1, memory_order_relaxed);
        }
    }
    inst->al_poll_idx = ecat_master_al_poll_send(inst);

    inst->cycle_counter++;

    bool wkc_error = (wkc < inst->expected_wkc);
    bool noframe   = (wkc == EC_NOFRAME);

    /* Single writer, relaxed atomics; readers tolerate cross-field tearing. Measures send+receive only. */
    atomic_store_explicit(&inst->diag.bus_cycle_ns, exchange_ns, memory_order_relaxed);

    if (wkc_error) {
        atomic_fetch_add_explicit(&inst->diag.wkc_error_count, 1,
                                  memory_order_relaxed);
        if (noframe)
            atomic_fetch_add_explicit(&inst->diag.noframe_count, 1,
                                      memory_order_relaxed);
    }

    atomic_fetch_add_explicit(&inst->diag.cycle_count, 1, memory_order_relaxed);

    /* EWMA kept as a sum (sum += x - sum/N, avg = sum/N): the avg form stalls when delta < N. */
    int64_t cur_sum = atomic_load_explicit(&inst->diag.avg_bus_cycle_ns_sum,
                                           memory_order_relaxed);
    cur_sum += (int64_t)exchange_ns - cur_sum / inst->avg_window;
    atomic_store_explicit(&inst->diag.avg_bus_cycle_ns_sum, cur_sum,
                          memory_order_relaxed);

    /* Min/max: single-writer, no CAS needed. */
    uint64_t cur = atomic_load_explicit(&inst->diag.max_bus_cycle_ns,
                                        memory_order_relaxed);
    if (exchange_ns > cur)
        atomic_store_explicit(&inst->diag.max_bus_cycle_ns, exchange_ns,
                              memory_order_relaxed);

    cur = atomic_load_explicit(&inst->diag.min_bus_cycle_ns,
                               memory_order_relaxed);
    if (exchange_ns < cur)
        atomic_store_explicit(&inst->diag.min_bus_cycle_ns, exchange_ns,
                              memory_order_relaxed);

    /* No logging here; the monitor reports these counters */
    int consec = 0;
    if (wkc_error) {
        consec = atomic_fetch_add_explicit(&inst->consecutive_wkc_errors, 1,
                                           memory_order_relaxed) + 1;
    } else {
        atomic_store(&inst->consecutive_wkc_errors, 0);
    }
#if ECAT_ENABLE_MONITOR_THREAD
    if (state == ECAT_STATE_OPERATIONAL && consec >= ECAT_WKC_ERROR_THRESHOLD) {
        atomic_store(&inst->recovery_al_trigger, 0);
        atomic_store(&inst->bus_state, ECAT_STATE_RECOVERING);
    } else if (state == ECAT_STATE_OPERATIONAL &&
               inst->consecutive_al_faults >= ECAT_AL_FAULT_THRESHOLD) {
        atomic_store(&inst->recovery_al_trigger,
                     0x10000u | atomic_load_explicit(&inst->al_status, memory_order_relaxed));
        atomic_store(&inst->bus_state, ECAT_STATE_RECOVERING);
    }
#else
    (void)consec;
#endif
    return true;
}

/* SIGUSR1 interrupts clock_nanosleep on stop; its handler is installed once in main.c. */

static inline uint64_t ts_to_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

/**
 * @brief Bus thread body — periodic SOEM exchange driver
 *
 * Runs at SCHED_FIFO with the configured task_priority. Sleeps absolutely
 * (CLOCK_MONOTONIC + TIMER_ABSTIME) to the next deadline so jitter stays
 * bounded. Each tick:
 *   - clock_nanosleep TIMER_ABSTIME → wake at the absolute deadline
 *   - capture wake-up timing: latency vs deadline, period vs prev wake
 *   - run the cycle (mutex+exchange+mutex)
 *   - advance the deadline
 *
 * Two scheduling metrics surface the answers to "are we hitting our
 * configured cycle time, and how late are we waking up?":
 *   - period_ns: actual_wake[N] - actual_wake[N-1]; should equal
 *     interval_ns on average on a healthy RT system.
 *   - latency_ns: actual_wake[N] - expected_wake[N]; how much later
 *     than its deadline the bus thread actually started running.
 *
 * Updates use the same lock-free atomic + time-based EWMA scheme as
 * bus_cycle_ns (see ECAT_AVG_TARGET_WINDOW_NS).  Single-writer (this
 * thread); JSON readers pull the values lock-free.
 */
static void *ecat_bus_thread(void *arg)
{
    ecat_master_instance_t *inst = (ecat_master_instance_t *)arg;

    /* Set thread name for top/htop debugging. */
    char tname[16];
    snprintf(tname, sizeof tname, "ecat-%.10s", inst->name);
    pthread_setname_np(pthread_self(), tname);

    /* Apply SCHED_FIFO at the configured priority. Fall back to the
     * default scheduler with a warning rather than refusing to run. */
    int prio = inst->config.master.task_priority;
    if (prio < 1)  prio = 1;
    if (prio > 99) prio = 99;
    struct sched_param sp = {0};
    sp.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        edog_log_warn(&g_logger,
            "Bus thread '%s': SCHED_FIFO(%d) failed: %s — running with default scheduling",
            inst->name, prio, strerror(errno));
    } else {
        edog_log_info(&g_logger,
            "Bus thread '%s': SCHED_FIFO priority %d", inst->name, prio);
    }

    /* SIGUSR1 (installed in main.c) is not blocked here, so stop can interrupt the sleep. */

    int64_t interval_ns =
        (int64_t)inst->config.master.cycle_time_us * 1000LL;
    if (interval_ns <= 0) interval_ns = 1000000LL; /* 1 ms safety floor */

    /* Seed scheduling-stat min trackers. */
    atomic_store_explicit(&inst->diag.min_period_ns,  UINT64_MAX, memory_order_relaxed);
    atomic_store_explicit(&inst->diag.min_latency_ns, INT64_MAX,  memory_order_relaxed);

    struct timespec next_wakeup;
    clock_gettime(CLOCK_MONOTONIC, &next_wakeup);

    bool     have_prev_wake = false;
    uint64_t prev_wake_ns   = 0;

    while (atomic_load(&inst->bus_running)) {
        /* Capture actual wake-up time. The first iteration's deadline
         * is "now" so latency should be ~0; meaningful from iteration 2. */
        struct timespec actual_wake;
        clock_gettime(CLOCK_MONOTONIC, &actual_wake);
        uint64_t actual_wake_ns = ts_to_ns(&actual_wake);
        uint64_t expected_ns    = ts_to_ns(&next_wakeup);

        /* Wake-up delay vs the deadline; signed, since an early wake is slightly negative. */
        int64_t latency_ns = (int64_t)actual_wake_ns - (int64_t)expected_ns;
        atomic_store_explicit(&inst->diag.latency_ns, latency_ns, memory_order_relaxed);

        int64_t cur_lat_min = atomic_load_explicit(&inst->diag.min_latency_ns,
                                                   memory_order_relaxed);
        if (latency_ns < cur_lat_min)
            atomic_store_explicit(&inst->diag.min_latency_ns, latency_ns,
                                  memory_order_relaxed);
        int64_t cur_lat_max = atomic_load_explicit(&inst->diag.max_latency_ns,
                                                   memory_order_relaxed);
        if (latency_ns > cur_lat_max)
            atomic_store_explicit(&inst->diag.max_latency_ns, latency_ns,
                                  memory_order_relaxed);

        /* Time-based EWMA — same scheme as avg_bus_cycle_ns_sum. */
        int64_t cur_lat_sum = atomic_load_explicit(&inst->diag.avg_latency_ns_sum,
                                                   memory_order_relaxed);
        cur_lat_sum += latency_ns - cur_lat_sum / inst->avg_window;
        atomic_store_explicit(&inst->diag.avg_latency_ns_sum, cur_lat_sum,
                              memory_order_relaxed);

        if (have_prev_wake) {
            uint64_t period_ns = actual_wake_ns - prev_wake_ns;
            atomic_store_explicit(&inst->diag.period_ns, period_ns,
                                  memory_order_relaxed);

            uint64_t cur_per_min = atomic_load_explicit(&inst->diag.min_period_ns,
                                                        memory_order_relaxed);
            if (period_ns < cur_per_min)
                atomic_store_explicit(&inst->diag.min_period_ns, period_ns,
                                      memory_order_relaxed);
            uint64_t cur_per_max = atomic_load_explicit(&inst->diag.max_period_ns,
                                                        memory_order_relaxed);
            if (period_ns > cur_per_max)
                atomic_store_explicit(&inst->diag.max_period_ns, period_ns,
                                      memory_order_relaxed);

            int64_t cur_per_sum = atomic_load_explicit(&inst->diag.avg_period_ns_sum,
                                                       memory_order_relaxed);
            cur_per_sum += (int64_t)period_ns - cur_per_sum / inst->avg_window;
            atomic_store_explicit(&inst->diag.avg_period_ns_sum, cur_per_sum,
                                  memory_order_relaxed);
        }
        prev_wake_ns   = actual_wake_ns;
        have_prev_wake = true;

        ecat_run_one_cycle(inst);

        next_wakeup.tv_nsec += (long)(interval_ns % 1000000000LL);
        next_wakeup.tv_sec  += (time_t)(interval_ns / 1000000000LL);
        if (next_wakeup.tv_nsec >= 1000000000L) {
            next_wakeup.tv_nsec -= 1000000000L;
            next_wakeup.tv_sec  += 1;
        }

        struct timespec done;
        clock_gettime(CLOCK_MONOTONIC, &done);
        if (ts_to_ns(&done) > ts_to_ns(&next_wakeup))
            atomic_fetch_add_explicit(&inst->overruns, 1, memory_order_relaxed);
        int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wakeup, NULL);
        if (rc == EINTR) continue; /* SIGUSR1 wake — loop will re-check bus_running */
    }

    edog_log_info(&g_logger,
        "Bus thread '%s': stopped after %llu cycles",
        inst->name,
        (unsigned long long)atomic_load_explicit(&inst->diag.cycle_count,
                                                 memory_order_relaxed));
    return NULL;
}

/*
 * =============================================================================
 * Lifecycle: configure / start / stop / shutdown
 * =============================================================================
 */

void ecat_bus_init(void)
{
    edog_logger_init(&g_logger, "BUS");
    ecat_config_set_logger(&g_logger);
}

void ecat_bus_set_state_dir(const char *dir)
{
    if (dir != NULL && dir[0] != '\0')
        snprintf(g_state_dir, sizeof(g_state_dir), "%s", dir);
    ecat_iface_state_set_dir(g_state_dir);
}

bool ecat_bus_any_active(void)
{
    for (int i = 0; i < g_master_count; i++) {
        int state = atomic_load(&g_masters[i].bus_state);
        if (state != ECAT_STATE_IDLE && state != ECAT_STATE_STOPPED && state != ECAT_STATE_ERROR)
            return true;
    }
    return false;
}

static void free_masters(void)
{
    for (int i = 0; i < g_master_count; i++) {
        ecat_master_instance_t *inst = &g_masters[i];
        ecat_data_destroy(inst);
        pthread_mutex_destroy(&inst->slaves_mutex);
    }
    free(g_masters);
    g_masters = NULL;
    g_master_count = 0;
}

static int init_instance_locks(ecat_master_instance_t *inst)
{
    if (ecat_mutex_init_pi(&inst->slaves_mutex) != 0)
        return -1;
    if (ecat_data_init(inst) != 0) {
        pthread_mutex_destroy(&inst->slaves_mutex);
        return -1;
    }
    return 0;
}

int ecat_bus_configure(const char *path, char *err, size_t err_size)
{
    if (ecat_bus_any_active()) {
        snprintf(err, err_size, "a master is running; stop the bus before reconfiguring");
        return -1;
    }

    if (path == NULL || path[0] == '\0') {
        free_masters();
        g_config_path[0] = '\0';
        edog_log_info(&g_logger, "Configuration cleared");
        return 0;
    }

    ecat_master_instance_t *temp = calloc(ECAT_MAX_MASTERS, sizeof(ecat_master_instance_t));
    if (temp == NULL) {
        snprintf(err, err_size, "out of memory");
        return -1;
    }

    int count = 0;
    int result = ecat_config_parse_all(path, temp, ECAT_MAX_MASTERS, &count);
    if (result != ECAT_CONFIG_OK || count == 0) {
        snprintf(err, err_size, "invalid bus configuration '%s' (code %d, %d master(s))", path,
                 result, count);
        free(temp);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        if (init_instance_locks(&temp[i]) != 0) {
            for (int j = 0; j < i; j++) {
                ecat_data_destroy(&temp[j]);
                pthread_mutex_destroy(&temp[j].slaves_mutex);
            }
            free(temp);
            snprintf(err, err_size, "failed to initialize master locks");
            return -1;
        }
        diag_reset(&temp[i].diag);
        atomic_store(&temp[i].bus_state, ECAT_STATE_IDLE);
    }

    free_masters();
    g_masters = temp;
    g_master_count = count;
    snprintf(g_config_path, sizeof(g_config_path), "%s", path);

    for (int i = 0; i < g_master_count; i++) {
        const ecat_master_instance_t *inst = &g_masters[i];
        edog_log_info(&g_logger, "Master[%d] '%s': interface=%s, cycle_time=%d us, slaves=%d", i,
                      inst->name, inst->config.master.interface, inst->config.master.cycle_time_us,
                      inst->config.slave_count);
    }
    edog_log_info(&g_logger, "Configuration loaded from %s: %d master(s)", path, g_master_count);
    return 0;
}

int ecat_bus_start(char *err, size_t err_size)
{
    if (g_master_count == 0) {
        snprintf(err, err_size, "no bus configuration loaded");
        return -1;
    }

    int started = 0;
    for (int i = 0; i < g_master_count; i++) {
        ecat_master_instance_t *inst = &g_masters[i];
        int state = atomic_load(&inst->bus_state);
        if (state == ECAT_STATE_OPERATIONAL || state == ECAT_STATE_RECOVERING) {
            started++; /* already running: start is idempotent */
            continue;
        }
        if (state != ECAT_STATE_IDLE && state != ECAT_STATE_STOPPED && state != ECAT_STATE_ERROR) {
            edog_log_error(&g_logger, "Master '%s': cannot start from state %s", inst->name,
                           ecat_state_to_string(state));
            continue;
        }
        if (start_single_master(inst) == 0)
            started++;
    }

    if (started == 0) {
        snprintf(err, err_size, "no EtherCAT master started (see EtherDOG log)");
        return -1;
    }
    edog_log_info(&g_logger, "%d/%d EtherCAT master(s) running", started, g_master_count);
    return started;
}

void ecat_bus_stop(void)
{
    for (int i = 0; i < g_master_count; i++)
        stop_single_master(&g_masters[i]);
}

void ecat_bus_shutdown(void)
{
    ecat_bus_stop();
    free_masters();
}

/*
 * =============================================================================
 * Async Command Handling (execute_command)
 * =============================================================================
 */

/**
 * @brief Validate a network interface name for safe use in SOEM calls.
 *
 * Accepts Linux names (e.g. "eth0") and Windows NPF device paths
 * (e.g. "\\Device\\NPF_{GUID}").  Writes a JSON error to @p out
 * on failure.
 *
 * @return 0 if valid, -1 if invalid (@p out already filled)
 */
/* --- command replies: heap strings, so a reply has no size limit ---------------------------- */

typedef struct {
    char *text;
} ecat_reply_t;

static void reply_set(ecat_reply_t *out, char *text)
{
    free(out->text);
    out->text = text;
}

static void reply_printf(ecat_reply_t *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void reply_printf(ecat_reply_t *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *text = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (text != NULL) {
        va_start(ap, fmt);
        vsnprintf(text, (size_t)n + 1, fmt, ap);
        va_end(ap);
    }
    reply_set(out, text);
}

/* Takes ownership of @p resp. */
static int reply_json(ecat_reply_t *out, cJSON *resp)
{
    reply_set(out, cJSON_PrintUnformatted(resp));
    cJSON_Delete(resp);
    return out->text != NULL ? 0 : -1;
}

static int validate_interface_name(const char *ifname, ecat_reply_t *out)
{
    size_t ifname_len = strlen(ifname);
    if (ifname_len == 0 || ifname_len >= ECAT_IFNAME_MAX) {
        reply_printf(out, "{\"error\":\"invalid interface name length\"}");
        return -1;
    }
    if (!ecat_iface_validate(ifname, ECAT_IFACE_ANY_PLATFORM)) {
        reply_printf(out, "{\"error\":\"invalid interface name format\"}");
        return -1;
    }
    return 0;
}

/**
 * @brief Check if any master is actively running on the bus
 *
 * Used by scan/test commands to refuse operation while masters are active.
 */
static bool any_master_active(void)
{
    for (int i = 0; i < g_master_count; i++) {
        int state = atomic_load(&g_masters[i].bus_state);
        if (state == ECAT_STATE_OPERATIONAL || state == ECAT_STATE_RECOVERING ||
            state == ECAT_STATE_TRANSITIONING) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Handle the "scan" command using a temporary SOEM context
 *
 * Creates a separate ecx_contextt (not the master's context) to scan
 * the bus for slaves. This allows scanning even when the master is not running.
 */
static int handle_scan_command(cJSON *root, ecat_reply_t *out)
{
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    cJSON *iface = params ? cJSON_GetObjectItemCaseSensitive(params, "interface") : NULL;

    if (!iface || !cJSON_IsString(iface)) {
        reply_printf(out, "{\"error\":\"missing 'interface' param\"}");
        return -1;
    }

    if (validate_interface_name(iface->valuestring, out) != 0)
        return -1;

    /* Refuse scan while any master is actively running on the bus */
    if (any_master_active()) {
        reply_printf(out, "{\"error\":\"EtherCAT master is running. Stop the bus before scanning.\"}");
        return -1;
    }

    /* Temporary SOEM context for scan (independent from master contexts) */
    ecx_contextt scan_ctx;
    memset(&scan_ctx, 0, sizeof(scan_ctx));

    if (!ecx_init(&scan_ctx, iface->valuestring)) {
        reply_printf(out, "{\"error\":\"Failed to open interface '%s'\"}", iface->valuestring);
        return -1;
    }

    int slave_count = ecx_config_init(&scan_ctx);
    if (slave_count <= 0) {
        ecx_close(&scan_ctx);
        reply_printf(out, "{\"status\":\"success\",\"devices\":[],\"message\":\"No slaves found\",\"slave_count\":0}");
        return 0;
    }

    /* Build JSON response with discovered slaves */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "success");
    cJSON *devices = cJSON_AddArrayToObject(resp, "devices");

    for (int i = 1; i <= scan_ctx.slavecount; i++) {
        ec_slavet *s = &scan_ctx.slavelist[i];
        cJSON *dev = cJSON_CreateObject();
        cJSON_AddNumberToObject(dev, "position", i);
        cJSON_AddStringToObject(dev, "name", s->name);
        cJSON_AddNumberToObject(dev, "vendor_id", s->eep_man);
        cJSON_AddNumberToObject(dev, "product_code", s->eep_id);
        cJSON_AddNumberToObject(dev, "revision", s->eep_rev);
        cJSON_AddNumberToObject(dev, "serial_number", s->eep_ser);
        cJSON_AddStringToObject(dev, "state", "UNKNOWN");
        cJSON_AddNumberToObject(dev, "al_status_code", 0);
        cJSON_AddBoolToObject(dev, "has_coe", (s->mbx_proto & 0x04) != 0);
        cJSON_AddNumberToObject(dev, "input_bytes", 0);
        cJSON_AddNumberToObject(dev, "output_bytes", 0);
        cJSON_AddItemToArray(devices, dev);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "Found %d EtherCAT slave(s)", scan_ctx.slavecount);
    cJSON_AddStringToObject(resp, "message", msg);
    cJSON_AddNumberToObject(resp, "slave_count", scan_ctx.slavecount);

    reply_set(out, cJSON_PrintUnformatted(resp));
    cJSON_Delete(resp);

    ecx_close(&scan_ctx);
    return 0;
}

/**
 * @brief Handle the "list-interfaces" command
 *
 * Uses ec_find_adapters() from SOEM to enumerate network adapters.
 * Does not require a SOEM context or bus access.
 */
static int handle_list_interfaces_command(ecat_reply_t *out)
{
    ec_adaptert *adapters = ec_find_adapters();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "success");
    cJSON *ifaces = cJSON_AddArrayToObject(resp, "interfaces");

    int count = 0;
    for (ec_adaptert *a = adapters; a != NULL; a = a->next) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", a->name);
        cJSON_AddStringToObject(entry, "description", a->desc);
        cJSON_AddItemToArray(ifaces, entry);
        count++;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "Found %d network interface(s)", count);
    cJSON_AddStringToObject(resp, "message", msg);

    reply_set(out, cJSON_PrintUnformatted(resp));
    cJSON_Delete(resp);
    ec_free_adapters(adapters);

    return 0;
}

/**
 * @brief Handle the "test" command using a temporary SOEM context
 *
 * Scans with a separate ecx_contextt and reports the slave at the requested position.
 */
static int handle_test_command(cJSON *root, ecat_reply_t *out)
{
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    cJSON *iface = params ? cJSON_GetObjectItemCaseSensitive(params, "interface") : NULL;
    cJSON *pos = params ? cJSON_GetObjectItemCaseSensitive(params, "position") : NULL;

    if (!iface || !cJSON_IsString(iface)) {
        reply_printf(out, "{\"error\":\"missing 'interface' param\"}");
        return -1;
    }
    if (!pos || !cJSON_IsNumber(pos)) {
        reply_printf(out, "{\"error\":\"missing 'position' param\"}");
        return -1;
    }

    int position = pos->valueint;
    if (position < 1) {
        reply_printf(out, "{\"error\":\"'position' must be a positive integer\"}");
        return -1;
    }

    if (validate_interface_name(iface->valuestring, out) != 0)
        return -1;

    /* Refuse test while any master is actively running on the bus */
    if (any_master_active()) {
        reply_printf(out, "{\"error\":\"EtherCAT master is running. Stop the bus before testing.\"}");
        return -1;
    }

    ecx_contextt test_ctx;
    memset(&test_ctx, 0, sizeof(test_ctx));

    if (!ecx_init(&test_ctx, iface->valuestring)) {
        reply_printf(out, "{\"error\":\"Failed to open interface '%s'\"}", iface->valuestring);
        return -1;
    }

    int slave_count = ecx_config_init(&test_ctx);
    if (slave_count <= 0) {
        ecx_close(&test_ctx);
        reply_printf(out, "{\"status\":\"success\",\"connected\":false,\"device\":null,"
            "\"message\":\"No EtherCAT slaves found on the network\"}");
        return 0;
    }

    if (position > test_ctx.slavecount) {
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg),
            "No device at position %d. Found %d slave(s).",
            position, test_ctx.slavecount);
        ecx_close(&test_ctx);
        reply_printf(out, "{\"status\":\"error\",\"connected\":false,\"device\":null,"
            "\"message\":\"%s\"}", errmsg);
        return -1;
    }

    ec_slavet *s = &test_ctx.slavelist[position];

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "success");
    cJSON_AddBoolToObject(resp, "connected", 1);

    cJSON *dev = cJSON_CreateObject();
    cJSON_AddNumberToObject(dev, "position", position);
    cJSON_AddStringToObject(dev, "name", s->name);
    cJSON_AddNumberToObject(dev, "vendor_id", s->eep_man);
    cJSON_AddNumberToObject(dev, "product_code", s->eep_id);
    cJSON_AddNumberToObject(dev, "revision", s->eep_rev);
    cJSON_AddNumberToObject(dev, "serial_number", s->eep_ser);
    cJSON_AddStringToObject(dev, "state", "UNKNOWN");
    cJSON_AddNumberToObject(dev, "al_status_code", 0);
    cJSON_AddBoolToObject(dev, "has_coe", (s->mbx_proto & 0x04) != 0);
    cJSON_AddNumberToObject(dev, "input_bytes", 0);
    cJSON_AddNumberToObject(dev, "output_bytes", 0);
    cJSON_AddItemToObject(resp, "device", dev);

    char msg[128];
    snprintf(msg, sizeof(msg),
        "Successfully connected to %s at position %d", s->name, position);
    cJSON_AddStringToObject(resp, "message", msg);

    reply_set(out, cJSON_PrintUnformatted(resp));
    cJSON_Delete(resp);

    ecx_close(&test_ctx);
    return 0;
}

/**
 * @brief Local view of cycle diag values, already converted ns -> us.
 *
 * Used only by the JSON builders to avoid repeating the same atomic_load
 * sequence twice.  Cross-field tearing is acceptable for diagnostics.
 */
typedef struct {
    uint64_t cycle_count;
    uint64_t wkc_error_count;
    uint64_t noframe_count;
    uint64_t avg_cycle_us;
    uint64_t min_cycle_us;
    uint64_t max_cycle_us;
    uint64_t min_exchange_us;
    uint64_t max_exchange_us;
    /* period: time between cycle starts. latency: wake-up delay vs the deadline (OS jitter). */
    int64_t  avg_period_us;
    int64_t  max_period_us;
    int64_t  min_period_us;
    int64_t  avg_latency_us;
    int64_t  max_latency_us;
    int64_t  min_latency_us;
} ecat_diag_view_t;

static void load_diag_view(const ecat_master_instance_t *inst,
                           ecat_diag_view_t *out)
{
    out->cycle_count = atomic_load_explicit(&inst->diag.cycle_count,
                                            memory_order_relaxed);
    out->wkc_error_count = atomic_load_explicit(&inst->diag.wkc_error_count,
                                                memory_order_relaxed);
    out->noframe_count = atomic_load_explicit(&inst->diag.noframe_count,
                                              memory_order_relaxed);

    /* avg = sum / avg_window. bus_cycle_ns is reported as both cycle_us and exchange_us. */
    int64_t window = inst->avg_window > 0 ? inst->avg_window : 1;

    int64_t bus_sum = atomic_load_explicit(&inst->diag.avg_bus_cycle_ns_sum,
                                           memory_order_relaxed);
    out->avg_cycle_us = (uint64_t)((bus_sum / window) / 1000);

    uint64_t min_bcn = atomic_load_explicit(&inst->diag.min_bus_cycle_ns,
                                            memory_order_relaxed);
    uint64_t max_bcn = atomic_load_explicit(&inst->diag.max_bus_cycle_ns,
                                            memory_order_relaxed);
    uint64_t min_us = (min_bcn == UINT64_MAX) ? 0 : min_bcn / 1000;
    uint64_t max_us = max_bcn / 1000;

    out->min_cycle_us    = min_us;
    out->max_cycle_us    = max_us;
    out->min_exchange_us = min_us;
    out->max_exchange_us = max_us;

    /* Scheduling stats — captured by the bus thread itself. */
    int64_t  per_sum    = atomic_load_explicit(&inst->diag.avg_period_ns_sum,
                                               memory_order_relaxed);
    uint64_t min_per_ns = atomic_load_explicit(&inst->diag.min_period_ns,
                                               memory_order_relaxed);
    uint64_t max_per_ns = atomic_load_explicit(&inst->diag.max_period_ns,
                                               memory_order_relaxed);
    int64_t  lat_sum    = atomic_load_explicit(&inst->diag.avg_latency_ns_sum,
                                               memory_order_relaxed);
    int64_t  min_lat_ns = atomic_load_explicit(&inst->diag.min_latency_ns,
                                               memory_order_relaxed);
    int64_t  max_lat_ns = atomic_load_explicit(&inst->diag.max_latency_ns,
                                               memory_order_relaxed);

    out->avg_period_us  = (per_sum / window) / 1000;
    out->max_period_us  = (int64_t)(max_per_ns / 1000);
    out->min_period_us  = (min_per_ns == UINT64_MAX) ? 0
                                                     : (int64_t)(min_per_ns / 1000);
    out->avg_latency_us = (lat_sum / window) / 1000;
    out->max_latency_us = max_lat_ns / 1000;
    out->min_latency_us = (min_lat_ns == INT64_MAX) ? 0 : min_lat_ns / 1000;
}

/**
 * @brief Add a "slaves" JSON array using the published slaves_snapshot.
 *
 * @param diagnostics Include extra fields (al_state_raw) when true.
 */
static void add_slaves_json(ecat_master_instance_t *inst, cJSON *master,
                            int *out_count, bool diagnostics)
{
    ecat_slave_status_t local[ECAT_MAX_SLAVES];
    int n;

    pthread_mutex_lock(&inst->slaves_mutex);
    n = inst->slaves_snapshot_count;
    if (n > ECAT_MAX_SLAVES)
        n = ECAT_MAX_SLAVES;
    memcpy(local, inst->slaves_snapshot, sizeof(local));
    pthread_mutex_unlock(&inst->slaves_mutex);

    *out_count = n;

    cJSON *slaves = cJSON_AddArrayToObject(master, "slaves");
    for (int i = 0; i < n; i++) {
        const ecat_slave_status_t *ss = &local[i];
        cJSON *slave = cJSON_CreateObject();
        cJSON_AddNumberToObject(slave, "position", ss->position);
        cJSON_AddStringToObject(slave, "name", ss->name);
        cJSON_AddStringToObject(slave, "state", al_state_to_string(ss->al_state));
        if (diagnostics)
            cJSON_AddNumberToObject(slave, "al_state_raw", ss->al_state);
        cJSON_AddNumberToObject(slave, "al_status_code", ss->al_status_code);
        cJSON_AddNumberToObject(slave, "error_count", ss->error_count);
        cJSON_AddBoolToObject(slave, "has_error",
                              (ss->al_state & EC_STATE_ERROR) != 0 ||
                              ss->al_state != EC_STATE_OPERATIONAL);
        cJSON_AddItemToArray(slaves, slave);
    }
}

/**
 * @brief Build a JSON object for a single master's status.
 *
 * Counters are read lock-free from inst->diag; slaves from the snapshot under slaves_mutex.
 */
static cJSON *build_master_status_json(ecat_master_instance_t *inst)
{
    ecat_bus_state_t state =
        (ecat_bus_state_t)atomic_load(&inst->bus_state);
    int consecutive_wkc = atomic_load(&inst->consecutive_wkc_errors);
    int recovery_attempts = atomic_load(&inst->recovery_attempts);
    uint64_t overruns = atomic_load(&inst->overruns);

    ecat_diag_view_t diag;
    load_diag_view(inst, &diag);

    cJSON *master = cJSON_CreateObject();
    cJSON_AddStringToObject(master, "name", inst->name);
    cJSON_AddStringToObject(master, "state", ecat_state_to_string(state));
    cJSON_AddNumberToObject(master, "expected_wkc", inst->expected_wkc);

    int slave_count = 0;
    add_slaves_json(inst, master, &slave_count, false);
    cJSON_AddNumberToObject(master, "slave_count", slave_count);

    /* Same keys as the diagnostics "timing" object. */
    cJSON *metrics = cJSON_CreateObject();
    cJSON_AddNumberToObject(metrics, "cycle_count", (double)diag.cycle_count);
    cJSON_AddNumberToObject(metrics, "wkc_error_count", (double)diag.wkc_error_count);
    cJSON_AddNumberToObject(metrics, "noframe_count", (double)diag.noframe_count);
    cJSON_AddNumberToObject(metrics, "avg_cycle_us", (double)diag.avg_cycle_us);
    cJSON_AddNumberToObject(metrics, "min_cycle_us", (double)diag.min_cycle_us);
    cJSON_AddNumberToObject(metrics, "max_cycle_us", (double)diag.max_cycle_us);
    cJSON_AddNumberToObject(metrics, "min_exchange_us", (double)diag.min_exchange_us);
    cJSON_AddNumberToObject(metrics, "max_exchange_us", (double)diag.max_exchange_us);
    cJSON_AddNumberToObject(metrics, "avg_period_us", (double)diag.avg_period_us);
    cJSON_AddNumberToObject(metrics, "max_period_us", (double)diag.max_period_us);
    cJSON_AddNumberToObject(metrics, "min_period_us", (double)diag.min_period_us);
    cJSON_AddNumberToObject(metrics, "avg_latency_us", (double)diag.avg_latency_us);
    cJSON_AddNumberToObject(metrics, "max_latency_us", (double)diag.max_latency_us);
    cJSON_AddNumberToObject(metrics, "min_latency_us", (double)diag.min_latency_us);
    cJSON_AddNumberToObject(metrics, "consecutive_wkc_errors", consecutive_wkc);
    cJSON_AddNumberToObject(metrics, "recovery_attempts", recovery_attempts);
    cJSON_AddNumberToObject(metrics, "overruns", (double)overruns);
    cJSON_AddItemToObject(master, "metrics", metrics);

    return master;
}

/**
 * @brief Handle the "status" command
 *
 * Returns a snapshot of all masters' status via the "masters" array.
 */
static int handle_status_command(ecat_reply_t *out)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "configured", g_master_count > 0);
    cJSON_AddStringToObject(resp, "config_path", g_config_path);

    cJSON *masters_arr = cJSON_AddArrayToObject(resp, "masters");
    for (int i = 0; i < g_master_count; i++) {
        cJSON_AddItemToArray(masters_arr, build_master_status_json(&g_masters[i]));
    }

    reply_set(out, cJSON_PrintUnformatted(resp));
    cJSON_Delete(resp);

    return 0;
}

/**
 * @brief Build a JSON object for a single master's diagnostics.
 */
static cJSON *build_master_diagnostics_json(ecat_master_instance_t *inst)
{
    ecat_bus_state_t state =
        (ecat_bus_state_t)atomic_load(&inst->bus_state);
    int consecutive_wkc = atomic_load(&inst->consecutive_wkc_errors);
    int recovery_attempts = atomic_load(&inst->recovery_attempts);
    uint64_t overruns = atomic_load(&inst->overruns);

    ecat_diag_view_t diag;
    load_diag_view(inst, &diag);

    cJSON *master = cJSON_CreateObject();
    cJSON_AddStringToObject(master, "name", inst->name);
    cJSON_AddStringToObject(master, "state", ecat_state_to_string(state));
    cJSON_AddNumberToObject(master, "expected_wkc", inst->expected_wkc);

    int slave_count = 0;
    add_slaves_json(inst, master, &slave_count, true);
    cJSON_AddNumberToObject(master, "slave_count", slave_count);

    /* cycle/exchange: work per cycle. period/latency: how well the bus thread is scheduled. */
    cJSON *timing = cJSON_CreateObject();
    cJSON_AddNumberToObject(timing, "cycle_count", (double)diag.cycle_count);
    cJSON_AddNumberToObject(timing, "wkc_error_count", (double)diag.wkc_error_count);
    cJSON_AddNumberToObject(timing, "noframe_count", (double)diag.noframe_count);
    cJSON_AddNumberToObject(timing, "avg_cycle_us", (double)diag.avg_cycle_us);
    cJSON_AddNumberToObject(timing, "min_cycle_us", (double)diag.min_cycle_us);
    cJSON_AddNumberToObject(timing, "max_cycle_us", (double)diag.max_cycle_us);
    cJSON_AddNumberToObject(timing, "min_exchange_us", (double)diag.min_exchange_us);
    cJSON_AddNumberToObject(timing, "max_exchange_us", (double)diag.max_exchange_us);
    cJSON_AddNumberToObject(timing, "avg_period_us", (double)diag.avg_period_us);
    cJSON_AddNumberToObject(timing, "max_period_us", (double)diag.max_period_us);
    cJSON_AddNumberToObject(timing, "min_period_us", (double)diag.min_period_us);
    cJSON_AddNumberToObject(timing, "avg_latency_us", (double)diag.avg_latency_us);
    cJSON_AddNumberToObject(timing, "max_latency_us", (double)diag.max_latency_us);
    cJSON_AddNumberToObject(timing, "min_latency_us", (double)diag.min_latency_us);
    cJSON_AddNumberToObject(timing, "configured_cycle_us",
                            inst->config.master.cycle_time_us);
    cJSON_AddNumberToObject(timing, "receive_timeout_us", inst->receive_timeout_us);
    cJSON_AddNumberToObject(timing, "overruns", (double)overruns);
    cJSON_AddItemToObject(master, "timing", timing);

    /* Recovery info */
    cJSON *recovery = cJSON_CreateObject();
    cJSON_AddNumberToObject(recovery, "consecutive_wkc_errors", consecutive_wkc);
    cJSON_AddNumberToObject(recovery, "recovery_attempts", recovery_attempts);
    cJSON_AddNumberToObject(recovery, "max_recovery_attempts", ECAT_MAX_RECOVERY_ATTEMPTS);
    cJSON_AddNumberToObject(recovery, "wkc_error_threshold", ECAT_WKC_ERROR_THRESHOLD);
    cJSON_AddNumberToObject(recovery, "writestate_failures",
        (double)atomic_load(&inst->recovery_writestate_failures));
    cJSON_AddItemToObject(master, "recovery", recovery);

    cJSON *al = cJSON_CreateObject();
    char al_hex[8];
    snprintf(al_hex, sizeof(al_hex), "0x%04X", (unsigned)atomic_load(&inst->al_status));
    cJSON_AddStringToObject(al, "status", al_hex);
    cJSON_AddNumberToObject(al, "responding", atomic_load(&inst->al_wkc));
    cJSON_AddNumberToObject(al, "replies", (double)atomic_load(&inst->al_replies));
    cJSON_AddNumberToObject(al, "misses", (double)atomic_load(&inst->al_misses));
    cJSON_AddNumberToObject(al, "faults", (double)atomic_load(&inst->al_faults));
    cJSON_AddItemToObject(master, "al_poll", al);

    /* Data session */
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "open", inst->data.open);
    cJSON_AddNumberToObject(data, "frames_rx",
        (double)atomic_load_explicit(&inst->data.frames_rx, memory_order_relaxed));
    cJSON_AddNumberToObject(data, "frames_tx",
        (double)atomic_load_explicit(&inst->data.frames_tx, memory_order_relaxed));
    cJSON_AddNumberToObject(data, "frames_dropped",
        (double)atomic_load_explicit(&inst->data.frames_dropped, memory_order_relaxed));
    cJSON_AddNumberToObject(data, "watchdog_trips",
        (double)atomic_load_explicit(&inst->data.watchdog_trips, memory_order_relaxed));
    cJSON_AddItemToObject(master, "data", data);

    /* Unlocked snapshot: a torn byte is acceptable in a diagnostic. */
    if (master_has_layout(inst)) {
        cJSON *image = cJSON_CreateObject();
        char hex[2 * 64 + 1];
        const uint8_t *regions[2] = { ecat_layout_output_region(inst), ecat_layout_input_region(inst) };
        uint32_t sizes[2] = { inst->layout.output_bytes, inst->layout.input_bytes };
        const char *keys[2] = { "outputs", "inputs" };
        for (int r = 0; r < 2; r++) {
            uint32_t n = sizes[r] < 64 ? sizes[r] : 64;
            for (uint32_t b = 0; b < n; b++)
                snprintf(hex + 2 * b, 3, "%02x", regions[r] ? regions[r][b] : 0);
            hex[2 * n] = '\0';
            cJSON_AddStringToObject(image, keys[r], hex);
        }
        cJSON_AddItemToObject(master, "process_image", image);
    }

    /* Master configuration */
    cJSON *master_cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(master_cfg, "interface", inst->config.master.interface);
    cJSON_AddNumberToObject(master_cfg, "cycle_time_us",
                            inst->config.master.cycle_time_us);
    cJSON_AddNumberToObject(master_cfg, "watchdog_timeout_cycles",
                            inst->config.master.watchdog_timeout_cycles);
    cJSON_AddItemToObject(master, "master_config", master_cfg);

    return master;
}

/**
 * @brief Handle the "diagnostics" command
 *
 * Returns detailed diagnostic information for all masters via the "masters" array.
 */
static int handle_diagnostics_command(ecat_reply_t *out)
{
    cJSON *resp = cJSON_CreateObject();

    cJSON *masters_arr = cJSON_AddArrayToObject(resp, "masters");
    for (int i = 0; i < g_master_count; i++) {
        cJSON_AddItemToArray(masters_arr,
                             build_master_diagnostics_json(&g_masters[i]));
    }

    reply_set(out, cJSON_PrintUnformatted(resp));
    cJSON_Delete(resp);

    return 0;
}

static int handle_configure_command(cJSON *root, ecat_reply_t *out)
{
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    cJSON *path = params ? cJSON_GetObjectItemCaseSensitive(params, "path") : NULL;
    if (path != NULL && !cJSON_IsString(path)) {
        reply_printf(out, "{\"error\":\"'path' must be a string\"}");
        return -1;
    }

    char err[512];
    if (ecat_bus_configure(path ? path->valuestring : NULL, err, sizeof(err)) != 0) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "error", err);
        reply_json(out, resp);
        return -1;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "success");
    cJSON *masters = cJSON_AddArrayToObject(resp, "masters");
    for (int i = 0; i < g_master_count; i++) {
        const ecat_master_instance_t *inst = &g_masters[i];
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "index", i);
        cJSON_AddStringToObject(m, "name", inst->name);
        cJSON_AddStringToObject(m, "interface", inst->config.master.interface);
        cJSON_AddNumberToObject(m, "cycle_time_us", inst->config.master.cycle_time_us);
        cJSON_AddNumberToObject(m, "task_priority", inst->config.master.task_priority);
        cJSON_AddNumberToObject(m, "slave_count", inst->config.slave_count);
        cJSON_AddItemToArray(masters, m);
    }
    return reply_json(out, resp);
}

static int handle_start_command(ecat_reply_t *out)
{
    char err[512];
    int started = ecat_bus_start(err, sizeof(err));
    cJSON *resp = cJSON_CreateObject();
    if (started < 0) {
        cJSON_AddStringToObject(resp, "error", err);
        reply_json(out, resp);
        return -1;
    }
    cJSON_AddStringToObject(resp, "status", "success");
    cJSON_AddNumberToObject(resp, "started", started);
    cJSON_AddNumberToObject(resp, "total", g_master_count);
    return reply_json(out, resp);
}

static int handle_stop_command(ecat_reply_t *out)
{
    ecat_bus_stop();
    reply_printf(out, "{\"status\":\"success\"}");
    return 0;
}

static bool master_has_layout(ecat_master_instance_t *inst)
{
    int state = atomic_load(&inst->bus_state);
    return state == ECAT_STATE_OPERATIONAL || state == ECAT_STATE_RECOVERING;
}

static int handle_layout_command(ecat_reply_t *out)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "success");
    cJSON *masters = cJSON_AddArrayToObject(resp, "masters");

    for (int i = 0; i < g_master_count; i++) {
        ecat_master_instance_t *inst = &g_masters[i];
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "index", i);
        cJSON_AddStringToObject(m, "name", inst->name);
        cJSON_AddStringToObject(m, "state",
            ecat_state_to_string((ecat_bus_state_t)atomic_load(&inst->bus_state)));
        bool ready = master_has_layout(inst);
        cJSON_AddBoolToObject(m, "ready", ready);
        cJSON_AddNumberToObject(m, "output_bytes", ready ? inst->layout.output_bytes : 0);
        cJSON_AddNumberToObject(m, "input_bytes", ready ? inst->layout.input_bytes : 0);
        cJSON *entries = cJSON_AddArrayToObject(m, "entries");
        for (int e = 0; ready && e < inst->layout.entry_count; e++) {
            const ecat_layout_entry_t *le = &inst->layout.entries[e];
            cJSON *je = cJSON_CreateObject();
            cJSON_AddNumberToObject(je, "slave", le->slave_position);
            cJSON_AddStringToObject(je, "pdo", le->pdo_index);
            cJSON_AddStringToObject(je, "index", le->entry_index);
            cJSON_AddNumberToObject(je, "subindex", le->entry_subindex);
            cJSON_AddStringToObject(je, "direction", le->is_output ? "output" : "input");
            cJSON_AddNumberToObject(je, "bit_offset", le->bit_offset);
            cJSON_AddNumberToObject(je, "bit_length", le->bit_length);
            cJSON_AddStringToObject(je, "data_type", ecat_data_type_to_string(le->data_type));
            cJSON_AddStringToObject(je, "name", le->name);
            cJSON_AddItemToArray(entries, je);
        }
        cJSON_AddItemToArray(masters, m);
    }
    return reply_json(out, resp);
}

static int handle_open_data_command(cJSON *root, ecat_reply_t *out)
{
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    cJSON *endpoint = params ? cJSON_GetObjectItemCaseSensitive(params, "endpoint") : NULL;
    if (endpoint == NULL || !cJSON_IsString(endpoint)) {
        reply_printf(out, "{\"error\":\"missing 'endpoint' param\"}");
        return -1;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *masters = cJSON_CreateArray();
    int opened = 0;
    for (int i = 0; i < g_master_count; i++) {
        ecat_master_instance_t *inst = &g_masters[i];
        if (!master_has_layout(inst))
            continue;
        char local[160];
        char err[256];
        uint64_t session = 0;
        if (ecat_data_open(inst, i, endpoint->valuestring, g_state_dir, local, sizeof(local),
                           &session, err, sizeof(err)) != 0) {
            cJSON_Delete(masters);
            cJSON_AddStringToObject(resp, "error", err);
            reply_json(out, resp);
            return -1;
        }
        char session_hex[17];
        snprintf(session_hex, sizeof(session_hex), "%016llx", (unsigned long long)session);
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "index", i);
        cJSON_AddStringToObject(m, "endpoint", local);
        cJSON_AddStringToObject(m, "session", session_hex);
        cJSON_AddItemToArray(masters, m);
        opened++;
    }

    if (opened == 0) {
        cJSON_Delete(masters);
        cJSON_AddStringToObject(resp, "error", "no master is running; start the bus first");
        reply_json(out, resp);
        return -1;
    }
    cJSON_AddStringToObject(resp, "status", "success");
    cJSON_AddItemToObject(resp, "masters", masters);
    return reply_json(out, resp);
}

static int handle_close_data_command(ecat_reply_t *out)
{
    for (int i = 0; i < g_master_count; i++)
        ecat_data_close(&g_masters[i]);
    reply_printf(out, "{\"status\":\"success\"}");
    return 0;
}

static int bus_command(const char *command_json, ecat_reply_t *out)
{
    cJSON *root = cJSON_Parse(command_json);
    if (!root) {
        reply_printf(out, "{\"error\":\"invalid JSON\"}");
        return -1;
    }

    cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cmd || !cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        reply_printf(out, "{\"error\":\"missing 'command' field\"}");
        return -1;
    }

    int result = -1;
    const char *name = cmd->valuestring;
    if (strcmp(name, "scan") == 0) {
        result = handle_scan_command(root, out);
    } else if (strcmp(name, "list-interfaces") == 0) {
        result = handle_list_interfaces_command(out);
    } else if (strcmp(name, "test") == 0) {
        result = handle_test_command(root, out);
    } else if (strcmp(name, "status") == 0) {
        result = handle_status_command(out);
    } else if (strcmp(name, "diagnostics") == 0) {
        result = handle_diagnostics_command(out);
    } else if (strcmp(name, "configure") == 0) {
        result = handle_configure_command(root, out);
    } else if (strcmp(name, "start") == 0) {
        result = handle_start_command(out);
    } else if (strcmp(name, "stop") == 0) {
        result = handle_stop_command(out);
    } else if (strcmp(name, "layout") == 0) {
        result = handle_layout_command(out);
    } else if (strcmp(name, "open_data") == 0) {
        result = handle_open_data_command(root, out);
    } else if (strcmp(name, "close_data") == 0) {
        result = handle_close_data_command(out);
    } else {
        cJSON *resp = cJSON_CreateObject();
        char msg[160];
        snprintf(msg, sizeof(msg), "unknown command '%.64s'", name);
        cJSON_AddStringToObject(resp, "error", msg);
        reply_json(out, resp);
    }

    cJSON_Delete(root);
    return result;
}

char *ecat_bus_command(const char *command_json)
{
    ecat_reply_t out = { NULL };
    bus_command(command_json, &out);
    if (out.text == NULL)
        reply_printf(&out, "{\"error\":\"no reply\"}");
    return out.text;
}
