// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file config.h
 * @brief Bus configuration structures, parser interface and per-master instance state.
 *
 * Mirrors the bus configuration JSON (docs/BUSCONFIG.md). The file describes the bus only:
 * masters, slaves, PDOs, SDOs and channels. How a client binds channels to its own variables
 * is the client's business and is not part of this format.
 */

#ifndef ETHERCAT_CONFIG_H
#define ETHERCAT_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

#include "dgram.h"
#include "log.h"
#include "soem/soem.h"

/* Maximum sizes */
#define ECAT_MAX_MASTERS      4
#define ECAT_MAX_SLAVES      64
#define ECAT_IOMAP_SIZE    8192
#define ECAT_MAX_CHANNELS    64
#define ECAT_MAX_PDO_ENTRIES 32
#define ECAT_MAX_PDOS        16
#define ECAT_MAX_SDOS        32
#define ECAT_MAX_NAME_LEN    64

/**
 * @brief Recognized EtherCAT/CoE data types
 *
 * Covers integer, floating-point, and padding types found in PDO entries.
 * REAL32 and REAL64 are transported through the existing DWORD/LWORD buffers
 * (IEEE 754 bit patterns preserved by memcpy).
 */
typedef enum {
    ECAT_DTYPE_UNKNOWN,
    ECAT_DTYPE_BOOL,
    ECAT_DTYPE_INT8,
    ECAT_DTYPE_UINT8,
    ECAT_DTYPE_INT16,
    ECAT_DTYPE_UINT16,
    ECAT_DTYPE_INT32,
    ECAT_DTYPE_UINT32,
    ECAT_DTYPE_INT64,
    ECAT_DTYPE_UINT64,
    ECAT_DTYPE_REAL32,
    ECAT_DTYPE_REAL64,
    ECAT_DTYPE_PAD
} ecat_data_type_t;

/* Error codes */
#define ECAT_CONFIG_OK              0
#define ECAT_CONFIG_ERR_FILE       -1
#define ECAT_CONFIG_ERR_PARSE      -2
#define ECAT_CONFIG_ERR_MEMORY     -3
#define ECAT_CONFIG_ERR_INVALID    -4
#define ECAT_CONFIG_ERR_MISSING    -5

/**
 * @brief PDO entry definition
 *
 * Represents a single entry within a PDO (Process Data Object).
 * Entries with index "0x0000" are padding entries.
 */
typedef struct {
    char             index[12];        /* hex string e.g. "0x6000" */
    uint8_t          subindex;
    uint8_t          bit_length;
    char             name[ECAT_MAX_NAME_LEN];
    ecat_data_type_t parsed_type;      /* resolved from data_type string in JSON */
} ecat_pdo_entry_t;

/**
 * @brief PDO (Process Data Object) definition
 *
 * Contains the PDO index and its list of entries.
 * RxPDOs are written to the slave, TxPDOs are read from the slave.
 */
typedef struct {
    char             index[12];  /* hex string e.g. "0x1A00" */
    char             name[ECAT_MAX_NAME_LEN];
    ecat_pdo_entry_t entries[ECAT_MAX_PDO_ENTRIES];
    int              entry_count;
} ecat_pdo_t;

/**
 * @brief SDO configuration entry
 *
 * Defines an SDO (Service Data Object) parameter to be written
 * to a slave during configuration phase.
 */
typedef struct {
    char             index[12];        /* hex string e.g. "0x8000" */
    uint8_t          subindex;
    double           value;            /* stored as double; cast to target type at write time */
    ecat_data_type_t parsed_type;      /* resolved from data_type string in JSON */
    char             name[ECAT_MAX_NAME_LEN];
} ecat_sdo_config_t;

/**
 * @brief Channel definition
 *
 * A named physical I/O point of a slave, linked to the PDO entry that carries it.
 */
typedef struct {
    int      index;
    char     name[ECAT_MAX_NAME_LEN];
    char     type[20];         /* "digital_input", "analog_output", etc. */
    uint8_t  bit_length;
    char     pdo_index[12];
    char     pdo_entry_index[12];
    uint8_t  pdo_entry_subindex;
} ecat_channel_t;

/**
 * @brief Per-slave startup checks
 *
 * Controls which identity fields are validated against the SOEM slave list
 * during topology verification.
 */
typedef struct {
    bool     check_vendor_id;
    bool     check_product_code;
} ecat_startup_checks_t;

/**
 * @brief Per-slave addressing
 *
 * Controls slave addressing on the bus.
 */
typedef struct {
    uint16_t ethercat_address;    /* 0 = auto-assign */
} ecat_addressing_t;

/**
 * @brief Per-slave timeouts
 *
 * Configurable timeouts for SDO operations and state transitions.
 */
typedef struct {
    int      sdo_timeout_ms;           /* SDO operation timeout (default: 1000) */
    int      init_to_preop_timeout_ms; /* INIT->PRE-OP timeout (default: 3000) */
    int      safeop_to_op_timeout_ms;  /* SAFE-OP->OP timeout (default: 10000) */
} ecat_timeouts_t;

/**
 * @brief Per-slave watchdog configuration
 *
 * Controls Sync Manager and PDI watchdog behavior per slave.
 */
typedef struct {
    bool     sm_watchdog_enabled;      /* Sync Manager watchdog */
    int      sm_watchdog_ms;           /* SM watchdog timeout (default: 100) */
    bool     pdi_watchdog_enabled;     /* PDI watchdog */
    int      pdi_watchdog_ms;          /* PDI watchdog timeout (default: 100) */
} ecat_watchdog_t;

/**
 * @brief Per-slave Distributed Clocks configuration
 *
 * Controls DC SYNC0/SYNC1 signal generation per slave.
 */
typedef struct {
    bool     enabled;
    int      sync_unit_cycle_us;       /* 0 = use master cycle */
    bool     sync0_enabled;
    int      sync0_cycle_us;
    int      sync0_shift_us;
    bool     sync1_enabled;
    int      sync1_cycle_us;
    int      sync1_shift_us;
} ecat_dc_config_t;

/**
 * @brief EtherCAT slave configuration
 *
 * Complete configuration for a single EtherCAT slave device,
 * including identity, channel mappings, PDOs, SDOs, and per-slave
 * settings for timeouts, watchdogs, and distributed clocks.
 */
typedef struct {
    int               position;        /* ec_slave[position] in SOEM (1-based) */
    char              name[ECAT_MAX_NAME_LEN];
    char              type[20];        /* "coupler", "digital_input", etc. */
    uint32_t          vendor_id;
    uint32_t          product_code;
    uint32_t          revision;
    ecat_channel_t    channels[ECAT_MAX_CHANNELS];
    int               channel_count;
    ecat_sdo_config_t sdo_configs[ECAT_MAX_SDOS];
    int               sdo_count;
    ecat_pdo_t        rx_pdos[ECAT_MAX_PDOS];
    int               rx_pdo_count;
    ecat_pdo_t        tx_pdos[ECAT_MAX_PDOS];
    int               tx_pdo_count;
    ecat_startup_checks_t startup_checks;
    ecat_addressing_t     addressing;
    ecat_timeouts_t       timeouts;
    ecat_watchdog_t       watchdog;
    ecat_dc_config_t      dc;
    /* Abort master startup on any SDO write failure (default true). */
    bool                  strict_sdo;
} ecat_slave_t;

/**
 * @brief Master configuration parameters
 */
/** Maximum interface name length including null terminator.
 *  Linux names fit in IFNAMSIZ (16), but Windows NPF device paths such as
 *  \\Device\\NPF_{GUID} can reach ~55 characters. */
#define ECAT_IFNAME_MAX 128

typedef struct {
    char             interface[ECAT_IFNAME_MAX];
    int              cycle_time_us;
    int              receive_timeout_us;
    int              watchdog_timeout_cycles;
    char             log_level[8];
    /** SCHED_FIFO priority for the dedicated bus thread (1-99).
     *  Defaults to 90 so the exchange is not starved by other real-time work. */
    int              task_priority;
    /* Zero outputs and confirm INIT transition on stop_loop (default true). */
    bool             safe_close;
} ecat_master_config_t;

/**
 * @brief Diagnostics configuration
 */
typedef struct {
    bool             log_connections;
    bool             log_data_access;
    bool             log_errors;
    int              max_log_entries;
    int              status_update_interval_ms;
} ecat_diagnostics_config_t;

/**
 * @brief Top-level bus configuration: master settings, slaves, diagnostics.
 *
 * About 7 MB (inline slave and PDO arrays): allocate statically or on the heap.
 */
typedef struct {
    ecat_master_config_t      master;
    ecat_slave_t              slaves[ECAT_MAX_SLAVES];
    int                       slave_count;
    ecat_diagnostics_config_t diagnostics;
} ecat_config_t;

/**
 * @brief Parse EtherCAT configuration from a JSON file
 *
 * Reads and parses a bus configuration JSON file.
 * The JSON has an array root format: [{ name, protocol, config }].
 * The "config" object is extracted and mapped to the ecat_config_t structure.
 *
 * @param config_path Path to the JSON configuration file
 * @param config Output configuration structure
 * @return ECAT_CONFIG_OK on success, negative error code on failure
 */
int ecat_config_parse(const char *config_path, ecat_config_t *config);

/**
 * @brief Validate a parsed configuration
 *
 * Checks required fields and value ranges.
 *
 * @param config Configuration to validate
 * @return ECAT_CONFIG_OK on success, negative error code on failure
 */
int ecat_config_validate(const ecat_config_t *config);

/**
 * @brief Provide a logger for the config parser to use for diagnostic messages.
 *
 * Optional — when not set, the parser falls back to stderr.  Idempotent.
 * The pointer must remain valid for the lifetime of any subsequent
 * ecat_config_parse* call (typically the lifetime of the process).
 */
void ecat_config_set_logger(edog_logger_t *logger);

/** Interface name rules: strict Linux names for NIC tuning, any socket-layer name (Windows NPF
 *  paths included) for scan and test. */
typedef enum {
    ECAT_IFACE_LINUX_STRICT,   /* alfanum + '_' '-', starts alpha, len 1..15 */
    ECAT_IFACE_ANY_PLATFORM    /* Linux + Windows NPF chars '\' '{' '}' '.' */
} ecat_iface_validate_mode_t;

/**
 * @brief Validate an interface name against the requested mode.
 *
 * @param iface NUL-terminated interface name (may be NULL — returns false).
 * @param mode  ECAT_IFACE_LINUX_STRICT or ECAT_IFACE_ANY_PLATFORM.
 * @return true if @p iface is a valid identifier under @p mode, false otherwise.
 */
bool ecat_iface_validate(const char *iface, ecat_iface_validate_mode_t mode);

/**
 * @brief Check whether an interface name is a safe Linux iface identifier.
 *
 * Thin wrapper kept for backward compatibility — equivalent to
 * ecat_iface_validate(iface, ECAT_IFACE_LINUX_STRICT).  Prefer the
 * explicit form in new code.
 *
 * @return true if safe, false otherwise.
 */
bool ecat_is_valid_iface_name(const char *iface);

/**
 * @brief Initialize configuration with default values
 *
 * @param config Configuration structure to initialize
 */
void ecat_config_init_defaults(ecat_config_t *config);

/**
 * @brief Parse a data type string into the corresponding enum value
 *
 * Recognizes standard CoE/EtherCAT type names and common aliases:
 *   "BOOL", "INT8"/"SINT", "UINT8"/"USINT", "INT16"/"INT",
 *   "UINT16"/"UINT", "INT32"/"DINT", "UINT32"/"UDINT",
 *   "INT64"/"LINT", "UINT64"/"ULINT",
 *   "REAL"/"REAL32"/"FLOAT", "LREAL"/"REAL64"/"DOUBLE", "PAD"
 *
 * @param str  NUL-terminated data type string (case-insensitive)
 * @return Matching ecat_data_type_t, or ECAT_DTYPE_UNKNOWN if unrecognized
 */
ecat_data_type_t ecat_parse_data_type(const char *str);

/*
 * =============================================================================
 * Bus State Machine
 * =============================================================================
 */

/** Maximum number of recovery attempts before transitioning to ERROR state */
#define ECAT_MAX_RECOVERY_ATTEMPTS  5

/** Number of consecutive WKC errors before triggering recovery */
#define ECAT_WKC_ERROR_THRESHOLD    3

/** 1: a low-priority monitor thread checks slave states and recovers them. 0: open loop. */
#ifndef ECAT_ENABLE_MONITOR_THREAD
#define ECAT_ENABLE_MONITOR_THREAD  1
#endif

/** Background monitor thread polling interval in milliseconds */
#define ECAT_MONITOR_INTERVAL_MS    500

/**
 * @brief Per-master bus state machine
 *
 * State transitions:
 *   STOPPED -> IDLE -> SCANNING -> CONFIGURING -> TRANSITIONING -> OPERATIONAL
 *   OPERATIONAL <-> RECOVERING
 *   RECOVERING -> ERROR (after max attempts)
 *   Any state -> STOPPED (via stop)
 */
typedef enum {
    ECAT_STATE_IDLE,           /* Configured, not started                   */
    ECAT_STATE_SCANNING,       /* ecx_init + ecx_config_init               */
    ECAT_STATE_CONFIGURING,    /* SDO writes + PDO mapping                 */
    ECAT_STATE_TRANSITIONING,  /* Slaves moving to SAFE-OP -> OP           */
    ECAT_STATE_OPERATIONAL,    /* Normal cyclic operation                   */
    ECAT_STATE_RECOVERING,     /* Attempting to recover slaves              */
    ECAT_STATE_ERROR,          /* Unrecoverable error                      */
    ECAT_STATE_STOPPED         /* After stop                               */
} ecat_bus_state_t;

/**
 * @brief Per-slave status snapshot for monitoring
 */
typedef struct {
    int      position;
    char     name[ECAT_MAX_NAME_LEN];
    uint16_t al_state;          /* EC_STATE_* from SOEM                    */
    uint16_t al_status_code;
    uint32_t error_count;
} ecat_slave_status_t;

/*
 * =============================================================================
 * Cycle Diagnostics
 * =============================================================================
 */

/** EWMA window in ns; N = window / cycle is stored in inst->avg_window. sum += x - sum/N,
 *  avg = sum/N. */
#define ECAT_AVG_TARGET_WINDOW_NS 2000000000LL

/**
 * @brief NIC settings saved by ecat_iface_state_apply() (coalescing, offloads) and restored by
 *        ecat_iface_state_revert(), or by the next start after a crash.
 */
typedef struct {
    char iface[ECAT_IFNAME_MAX];

    /* NIC tuning -- ethtool -C (coalescing) */
    bool coalescing_saved;
    int  rx_usecs;
    int  tx_usecs;

    /* NIC tuning -- ethtool -K (offloads) */
    bool offloads_saved;
    bool gro;
    bool gso;
    bool tso;
} ecat_iface_state_t;

/**
 * @brief Per-cycle timing, lock-free; single writer (bus thread).
 *
 * bus_cycle: send+receive time. period: gap between cycle starts. latency: wake-up delay vs the
 * deadline. avg_*_sum fields are EWMA sums over inst->avg_window samples.
 */
typedef struct {
    _Atomic(uint64_t) cycle_count;       /* total cycles executed              */
    _Atomic(uint64_t) wkc_error_count;   /* total WKC errors (wkc < expected)  */
    _Atomic(uint64_t) noframe_count;     /* total EC_NOFRAME (-1) errors       */

    /* Work timing -- bus exchange duration */
    _Atomic(uint64_t) bus_cycle_ns;          /* last send+receive duration (ns) */
    _Atomic(uint64_t) max_bus_cycle_ns;      /* worst-case send+receive         */
    _Atomic(uint64_t) min_bus_cycle_ns;      /* best-case send+receive          */
    _Atomic(int64_t)  avg_bus_cycle_ns_sum;  /* EWMA accumulator; avg = sum/N   */

    /* Scheduling timing -- period and wake-up latency */
    _Atomic(uint64_t) period_ns;             /* last observed cycle period (ns) */
    _Atomic(uint64_t) max_period_ns;         /* worst-case period               */
    _Atomic(uint64_t) min_period_ns;         /* best-case period                */
    _Atomic(int64_t)  avg_period_ns_sum;     /* EWMA accumulator; avg = sum/N   */
    _Atomic(int64_t)  latency_ns;            /* last wake-up scheduling delay   */
    _Atomic(int64_t)  max_latency_ns;        /* worst-case wake-up delay        */
    _Atomic(int64_t)  min_latency_ns;        /* best-case wake-up delay         */
    _Atomic(int64_t)  avg_latency_ns_sum;    /* EWMA accumulator; avg = sum/N   */
} ecat_cycle_diag_t;

/*
 * =============================================================================
 * Process Data Layout and Data Session
 * =============================================================================
 */

/* Maximum PDO entries published in one direction of a master's layout */
#define ECAT_MAX_LAYOUT_ENTRIES 512

/**
 * @brief One PDO entry as it sits in the published process image.
 *
 * bit_offset is relative to the start of its direction's region: outputs are
 * IOmap[0, output_bytes), inputs are IOmap[output_bytes, output_bytes + input_bytes).
 */
typedef struct {
    int              slave_position;          /* 1-based                           */
    char             pdo_index[12];           /* e.g. "0x1A00"                     */
    char             entry_index[12];         /* e.g. "0x6000"                     */
    uint8_t          entry_subindex;
    bool             is_output;               /* true: RxPDO (master -> slave)     */
    uint32_t         bit_offset;
    uint8_t          bit_length;
    ecat_data_type_t data_type;
    char             name[ECAT_MAX_NAME_LEN];
} ecat_layout_entry_t;

typedef struct {
    uint32_t            output_bytes;
    uint32_t            input_bytes;
    ecat_layout_entry_t entries[ECAT_MAX_LAYOUT_ENTRIES];
    int                 entry_count;
} ecat_layout_t;

/** Largest datagram payload: one direction's region must fit in one frame. */
#define ECAT_MAX_FRAME_PAYLOAD 4096

/**
 * @brief Cyclic data session with one client (see docs/PROTOCOL.md).
 *
 * The control thread opens and closes it under @ref lock; the bus thread only
 * trylocks, so a control request can never stall the exchange.
 */
typedef struct {
    pthread_mutex_t lock;
    bool            open;
    int             fd;                  /* non-blocking datagram socket       */
    edog_dgram_peer_t peer;              /* client endpoint                     */
    char            local_path[108];     /* AF_UNIX: our bound path, unlinked on close */
    uint64_t        session;
    uint32_t        tx_seq;              /* inputs sent                         */
    uint32_t        rx_last_seq;         /* last output frame applied           */
    bool            rx_seen;             /* any valid output frame this session */
    uint32_t        cycles_since_output; /* for the output watchdog             */
    bool            outputs_zeroed;      /* watchdog already cleared outputs    */
    _Atomic(uint64_t) frames_rx;
    _Atomic(uint64_t) frames_tx;
    _Atomic(uint64_t) frames_dropped;
    _Atomic(uint64_t) watchdog_trips;
} ecat_data_session_t;

/*
 * =============================================================================
 * Multi-Master Instance Structures
 * =============================================================================
 */

/** @brief All state of one master. About 7 MB: heap-allocate. */
typedef struct {
    /* Identity */
    char name[ECAT_MAX_NAME_LEN];       /* master name from JSON config */

    /* Configuration (parsed from JSON) */
    ecat_config_t config;

    /* SOEM context and IOmap — per-instance, NOT shared */
    ecx_contextt ecx_context;
    uint8_t iomap[ECAT_IOMAP_SIZE];
    int soem_initialized;
    size_t iomap_used_size;

    /* Published process data layout (built at start) and the client session */
    ecat_layout_t       layout;
    ecat_data_session_t data;

    /* State machine */
    _Atomic(int) bus_state;             /* ecat_bus_state_t */
    int expected_wkc;
    int receive_timeout_us;

    /* Diagnostics (updated by the bus thread) */
    ecat_cycle_diag_t diag;
    _Atomic(int) consecutive_wkc_errors;
    _Atomic(int) recovery_attempts;
    /* Counts ecx_writestate calls during recovery that returned wkc<=0
     * (request did not reach the slave -- link/cable issue, vs. slave
     * reachable but rejecting the state).  Distinguishes physical from
     * configuration recovery failures in the operator UI. */
    _Atomic(uint32_t) recovery_writestate_failures;
    uint64_t cycle_counter;

    /* Per-slave snapshot for status queries. slavelist[] is mutated by the monitor during
     * recovery; the monitor publishes the fields queries need here under slaves_mutex, so a
     * query never takes soem_lock and never costs the bus thread a cycle. */
    ecat_slave_status_t slaves_snapshot[ECAT_MAX_SLAVES];
    int                 slaves_snapshot_count;
    pthread_mutex_t     slaves_mutex;

#if ECAT_ENABLE_MONITOR_THREAD
    /* Monitor thread. soem_lock serializes SOEM access between the bus thread (trylock, skips
     * the cycle on contention) and the monitor (state checks, recovery). PRIO_INHERIT. */
    pthread_t monitor_thread;
    _Atomic(bool) monitor_running;
    pthread_mutex_t soem_lock;
    _Atomic(uint64_t) exchange_skips;
#endif

    /* Dedicated bus thread: periodic at master.cycle_time_us, SCHED_FIFO at task_priority. */
    pthread_t              bus_thread;
    _Atomic(bool)          bus_running;

    /* Time-based EWMA window in samples; computed from cycle_time_us at
     * start_single_master so the wall-clock smoothing window matches
     * ECAT_AVG_TARGET_WINDOW_NS regardless of configured cycle rate. */
    int64_t                avg_window;

    /* Per-iface external state (NIC tuning + IP-stack isolation).
     * Populated by ecat_iface_state_apply(); consumed by
     * ecat_iface_state_revert().  Includes its own iface name copy so
     * revert can run after config has been freed. */
    ecat_iface_state_t iface_state;
} ecat_master_instance_t;

/**
 * @brief Parse all EtherCAT master configurations from a JSON file
 *
 * Iterates ALL entries in the JSON array (not just the first).
 * Each entry with "protocol": "ETHERCAT" is parsed into a separate config.
 * Also extracts the master name from each entry.
 *
 * @param config_path  Path to the JSON configuration file
 * @param instances    Output array of master instances (only name + config are populated)
 * @param max_masters  Maximum number of masters to parse
 * @param out_count    Output: number of masters actually parsed
 * @return ECAT_CONFIG_OK on success, negative error code on failure
 */
int ecat_config_parse_all(const char *config_path,
                          ecat_master_instance_t *instances,
                          int max_masters,
                          int *out_count);

/**
 * @brief Convert a bus state to a human-readable string
 *
 * @param state Bus state value
 * @return Static string representation (e.g., "OPERATIONAL")
 */
const char *ecat_state_to_string(ecat_bus_state_t state);

/**
 * @brief Get the size in bytes for a given EtherCAT data type
 *
 * Used for SDO write operations to determine the payload size.
 *
 * @param dt Data type enum value
 * @return Size in bytes (0 for UNKNOWN/PAD, 1 for BOOL)
 */
int ecat_data_type_size(ecat_data_type_t dt);

/**
 * @brief Convert a data type enum to a human-readable name (e.g. "INT16").
 *
 * Symmetric with ecat_state_to_string.  Used in log messages so the
 * structs no longer need to carry the original JSON string.
 *
 * @param dt Data type enum value
 * @return Static string ("UNKNOWN" for invalid or unrecognized values)
 */
const char *ecat_data_type_to_string(ecat_data_type_t dt);

#endif /* ETHERCAT_CONFIG_H */
