// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file log.h
 * @brief Logger with three outputs: stderr, an in-memory ring, and an optional socket sink.
 *
 * Every entry lands in a ring of recent entries (queried by the "logs" command) and on
 * stderr. With a sink configured, a background thread also streams entries as JSON lines
 * ({"timestamp","level","source","message"}) to a log server, reconnecting when it goes away.
 * Callers never block on the sink. Still never log from the bus thread's cycle.
 */

#ifndef EDOG_LOG_H
#define EDOG_LOG_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EDOG_LOG_DEBUG = 0,
    EDOG_LOG_INFO,
    EDOG_LOG_WARN,
    EDOG_LOG_ERROR
} edog_log_level_t;

typedef struct {
    char tag[32];
} edog_logger_t;

/** One ring entry, as returned by edog_log_query. */
typedef struct {
    uint64_t id;
    int64_t timestamp; /* seconds since the epoch */
    edog_log_level_t level;
    char tag[32];
    char message[512];
} edog_log_entry_t;

#define EDOG_LOG_RING_SIZE 1000

/** Minimum level printed and recorded, process-wide. Defaults to INFO. */
void edog_log_set_level(edog_log_level_t level);

/** Parse "debug" / "info" / "warn" / "error"; returns false on anything else. */
bool edog_log_parse_level(const char *name, edog_log_level_t *out);

const char *edog_log_level_name(edog_log_level_t level);

/**
 * @brief Stream entries to a log server at @p spec ("unix:<path>" or "tcp:<ip>:<port>").
 *
 * Starts the sink thread; entries already in the ring are sent first. Returns 0, or -1 on a
 * malformed spec.
 */
int edog_log_start_sink(const char *spec);

/** Stop the sink thread (flushes what it can). */
void edog_log_stop_sink(void);

/**
 * @brief Copy entries with id >= @p min_id and level >= @p min_level, oldest first.
 * @return number of entries written to @p out (at most @p max).
 */
int edog_log_query(uint64_t min_id, edog_log_level_t min_level, edog_log_entry_t *out, int max);

/** Id the next entry will get. */
uint64_t edog_log_next_id(void);

void edog_logger_init(edog_logger_t *logger, const char *tag);

void edog_log_debug(edog_logger_t *logger, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void edog_log_info(edog_logger_t *logger, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void edog_log_warn(edog_logger_t *logger, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void edog_log_error(edog_logger_t *logger, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* EDOG_LOG_H */
