// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file config.c
 * @brief Bus configuration parser (cJSON). Format: docs/BUSCONFIG.md
 *   [{ "name": "ethercat_master", "protocol": "ETHERCAT", "config": { ... } }]
 */

#include "config.h"
#include "cjson/cJSON.h"

#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Diagnostic logging                                                 */
/* ------------------------------------------------------------------ */

/* Set once via ecat_config_set_logger() before any parse; read-only afterwards. */
static edog_logger_t *g_config_logger = NULL;

void ecat_config_set_logger(edog_logger_t *logger)
{
    g_config_logger = logger;
}

/** Linux IFNAMSIZ is 16; valid iface names are 1..15 chars. */
#define ECAT_LINUX_IFNAME_MAX 16

bool ecat_iface_validate(const char *iface, ecat_iface_validate_mode_t mode)
{
    if (iface == NULL)
        return false;
    size_t len = strlen(iface);
    if (len == 0)
        return false;

    if (mode == ECAT_IFACE_LINUX_STRICT) {
        if (len >= ECAT_LINUX_IFNAME_MAX)
            return false;
        if (!isalpha((unsigned char)iface[0]))
            return false;
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)iface[i];
            if (!isalnum(c) && c != '_' && c != '-')
                return false;
        }
        return true;
    }

    /* ECAT_IFACE_ANY_PLATFORM: Linux names + Windows NPF device paths */
    if (len >= ECAT_IFNAME_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)iface[i];
        if (!isalnum(c) && c != '_' && c != '-' &&
            c != '\\' && c != '{' && c != '}' && c != '.')
            return false;
    }
    return true;
}

bool ecat_is_valid_iface_name(const char *iface)
{
    return ecat_iface_validate(iface, ECAT_IFACE_LINUX_STRICT);
}

/*
 * =============================================================================
 * Helper Functions
 * =============================================================================
 */

/**
 * @brief Read entire file into a string
 */
static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0 || size > 1024 * 1024) { /* Max 1MB config file */
        fclose(fp);
        return NULL;
    }

    char *buffer = (char *)malloc(size + 1);
    if (buffer == NULL) {
        fclose(fp);
        return NULL;
    }

    size_t read_size = fread(buffer, 1, size, fp);
    fclose(fp);

    if ((long)read_size != size) {
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';
    return buffer;
}

/**
 * @brief Safely copy string with length limit
 */
static void safe_strcpy(char *dest, const char *src, size_t max_len)
{
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }
    strncpy(dest, src, max_len - 1);
    dest[max_len - 1] = '\0';
}

/**
 * @brief Get string value from JSON object
 */
static const char *get_string(const cJSON *obj, const char *key, const char *default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return item->valuestring;
    }
    return default_val;
}

/**
 * @brief Get integer value from JSON object
 */
static int get_int(const cJSON *obj, const char *key, int default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return default_val;
}

/**
 * @brief Get a numeric value from a JSON field that may be a number or a string.
 *
 * Handles:
 *   - JSON numbers  (cJSON_IsNumber)
 *   - Decimal strings ("100", "-50")
 *   - Hex strings    ("0xFF", "0x1A")
 *   - Float strings  ("3.14", "-1.5e2")
 *
 * @return The parsed value, or default_val on missing/empty/unparseable input.
 */
static double get_numeric_value(const cJSON *obj, const char *key, double default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (item == NULL)
        return default_val;

    if (cJSON_IsNumber(item))
        return item->valuedouble;

    if (cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != '\0') {
        const char *str = item->valuestring;
        char *endptr = NULL;

        /* Check for hex prefix */
        if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
            long long hex_val = strtoll(str, &endptr, 16);
            if (endptr != str && *endptr == '\0')
                return (double)hex_val;
            return default_val;
        }

        /* Try parsing as double (covers integers, floats, negative, scientific) */
        double dval = strtod(str, &endptr);
        if (endptr != str && *endptr == '\0')
            return dval;
    }

    return default_val;
}

/**
 * @brief Get boolean value from JSON object
 */
static bool get_bool(const cJSON *obj, const char *key, bool default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_val;
}

/**
 * @brief Convert hex string (e.g. "0x00000002") to uint32_t
 */
static uint32_t hex_to_uint32(const char *hex_str)
{
    if (hex_str == NULL) {
        return 0;
    }
    return (uint32_t)strtoul(hex_str, NULL, 16);
}

/**
 * @brief Case-insensitive string comparison
 */
static int strcasecmp_local(const char *a, const char *b)
{
    while (*a && *b) {
        int diff = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (diff != 0)
            return diff;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

ecat_data_type_t ecat_parse_data_type(const char *str)
{
    if (str == NULL || str[0] == '\0')
        return ECAT_DTYPE_UNKNOWN;

    /* Boolean */
    if (strcasecmp_local(str, "BOOL") == 0)
        return ECAT_DTYPE_BOOL;

    /* 8-bit integer */
    if (strcasecmp_local(str, "INT8") == 0 || strcasecmp_local(str, "SINT") == 0)
        return ECAT_DTYPE_INT8;
    if (strcasecmp_local(str, "UINT8") == 0 || strcasecmp_local(str, "USINT") == 0 ||
        strcasecmp_local(str, "BYTE") == 0)
        return ECAT_DTYPE_UINT8;

    /* 16-bit integer */
    if (strcasecmp_local(str, "INT16") == 0 || strcasecmp_local(str, "INT") == 0)
        return ECAT_DTYPE_INT16;
    if (strcasecmp_local(str, "UINT16") == 0 || strcasecmp_local(str, "UINT") == 0 ||
        strcasecmp_local(str, "WORD") == 0)
        return ECAT_DTYPE_UINT16;

    /* 32-bit integer */
    if (strcasecmp_local(str, "INT32") == 0 || strcasecmp_local(str, "DINT") == 0)
        return ECAT_DTYPE_INT32;
    if (strcasecmp_local(str, "UINT32") == 0 || strcasecmp_local(str, "UDINT") == 0 ||
        strcasecmp_local(str, "DWORD") == 0)
        return ECAT_DTYPE_UINT32;

    /* 64-bit integer */
    if (strcasecmp_local(str, "INT64") == 0 || strcasecmp_local(str, "LINT") == 0)
        return ECAT_DTYPE_INT64;
    if (strcasecmp_local(str, "UINT64") == 0 || strcasecmp_local(str, "ULINT") == 0 ||
        strcasecmp_local(str, "LWORD") == 0)
        return ECAT_DTYPE_UINT64;

    /* 32-bit float (REAL) */
    if (strcasecmp_local(str, "REAL") == 0 || strcasecmp_local(str, "REAL32") == 0 ||
        strcasecmp_local(str, "FLOAT") == 0)
        return ECAT_DTYPE_REAL32;

    /* 64-bit float (LREAL) */
    if (strcasecmp_local(str, "LREAL") == 0 || strcasecmp_local(str, "REAL64") == 0 ||
        strcasecmp_local(str, "DOUBLE") == 0)
        return ECAT_DTYPE_REAL64;

    /* Padding */
    if (strcasecmp_local(str, "PAD") == 0)
        return ECAT_DTYPE_PAD;

    return ECAT_DTYPE_UNKNOWN;
}

/*
 * =============================================================================
 * Section Parsers
 * =============================================================================
 */

/**
 * @brief Parse master configuration from JSON
 */
static void parse_master_section(const cJSON *master, ecat_master_config_t *config)
{
    /* Defaults applied even when the "master" object is absent. */
    config->safe_close = true;

    if (master == NULL) {
        return;
    }

    safe_strcpy(config->interface, get_string(master, "interface", "eth0"), sizeof(config->interface));
    config->cycle_time_us = get_int(master, "cycle_time_us", 1000);
    config->receive_timeout_us = get_int(master, "receive_timeout_us", 2000);
    config->watchdog_timeout_cycles = get_int(master, "watchdog_timeout_cycles", 3);
    safe_strcpy(config->log_level, get_string(master, "log_level", "info"), sizeof(config->log_level));
    config->task_priority = get_int(master, "task_priority", 90);
    config->safe_close = get_bool(master, "safe_close", true);
}

/**
 * @brief Parse diagnostics configuration from JSON
 */
static void parse_diagnostics_section(const cJSON *diag, ecat_diagnostics_config_t *config)
{
    if (diag == NULL) {
        return;
    }

    config->log_connections = get_bool(diag, "log_connections", true);
    config->log_data_access = get_bool(diag, "log_data_access", false);
    config->log_errors = get_bool(diag, "log_errors", true);
    config->max_log_entries = get_int(diag, "max_log_entries", 10000);
    config->status_update_interval_ms = get_int(diag, "status_update_interval_ms", 500);
}

/**
 * @brief Parse a single PDO entry from JSON
 */
static int parse_pdo_entry(const cJSON *entry_json, ecat_pdo_entry_t *entry)
{
    if (entry_json == NULL || entry == NULL) {
        return ECAT_CONFIG_ERR_INVALID;
    }

    safe_strcpy(entry->index, get_string(entry_json, "index", "0x0000"), sizeof(entry->index));
    entry->subindex = (uint8_t)get_int(entry_json, "subindex", 0);
    entry->bit_length = (uint8_t)get_int(entry_json, "bit_length", 0);
    safe_strcpy(entry->name, get_string(entry_json, "name", ""), sizeof(entry->name));
    entry->parsed_type = ecat_parse_data_type(get_string(entry_json, "data_type", ""));

    return ECAT_CONFIG_OK;
}

/**
 * @brief Parse a single PDO from JSON. Allocates pdo->entries sized from the JSON array.
 *
 * @p pdo must be zero-initialized; on OOM returns ECAT_CONFIG_ERR_MEMORY and leaves pdo
 * in a destroy-safe state (NULL entries, zero counts).
 */
static int parse_pdo(const cJSON *pdo_json, ecat_pdo_t *pdo)
{
    if (pdo_json == NULL || pdo == NULL) {
        return ECAT_CONFIG_ERR_INVALID;
    }

    safe_strcpy(pdo->index, get_string(pdo_json, "index", "0x0000"), sizeof(pdo->index));
    safe_strcpy(pdo->name, get_string(pdo_json, "name", ""), sizeof(pdo->name));

    pdo->entries = NULL;
    pdo->entry_count = 0;
    pdo->entry_capacity = 0;

    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(pdo_json, "entries");
    if (entries == NULL || !cJSON_IsArray(entries)) {
        return ECAT_CONFIG_OK;
    }

    int n = cJSON_GetArraySize(entries);
    if (n <= 0) {
        return ECAT_CONFIG_OK;
    }

    pdo->entries = (ecat_pdo_entry_t *)calloc((size_t)n, sizeof(ecat_pdo_entry_t));
    if (pdo->entries == NULL) {
        edog_log_error(g_config_logger, "PDO %s: out of memory allocating %d entries",
                       pdo->index, n);
        return ECAT_CONFIG_ERR_MEMORY;
    }
    pdo->entry_capacity = n;

    const cJSON *entry_json;
    cJSON_ArrayForEach(entry_json, entries) {
        if (parse_pdo_entry(entry_json, &pdo->entries[pdo->entry_count]) == ECAT_CONFIG_OK) {
            pdo->entry_count++;
        }
    }

    return ECAT_CONFIG_OK;
}

/**
 * @brief Parse an array of PDOs (rx_pdos or tx_pdos) from JSON. Allocates *pdos_out.
 *
 * The buffer in *pdos_out is sized from the JSON array length and populated in place. On any
 * error, the caller must call ecat_slave_destroy() (or walk+free the partially filled
 * entries) to release what was allocated.
 */
static int parse_pdo_array(const cJSON *pdo_array, ecat_pdo_t **pdos_out,
                           int *pdo_count, int *pdo_capacity)
{
    *pdos_out = NULL;
    *pdo_count = 0;
    *pdo_capacity = 0;

    if (pdo_array == NULL || !cJSON_IsArray(pdo_array)) {
        return ECAT_CONFIG_OK;
    }

    int n = cJSON_GetArraySize(pdo_array);
    if (n <= 0) {
        return ECAT_CONFIG_OK;
    }

    *pdos_out = (ecat_pdo_t *)calloc((size_t)n, sizeof(ecat_pdo_t));
    if (*pdos_out == NULL) {
        edog_log_error(g_config_logger, "out of memory allocating %d PDOs", n);
        return ECAT_CONFIG_ERR_MEMORY;
    }
    *pdo_capacity = n;

    const cJSON *pdo_json;
    cJSON_ArrayForEach(pdo_json, pdo_array) {
        int rc = parse_pdo(pdo_json, &(*pdos_out)[*pdo_count]);
        if (rc != ECAT_CONFIG_OK)
            return rc;
        (*pdo_count)++;
    }

    return ECAT_CONFIG_OK;
}

/**
 * @brief Check whether a JSON-derived double value fits the wire type.
 *
 * Catches NaN/Inf and out-of-range values that would silently wrap or
 * trigger UB during the cast in ecat_master_write_sdos.  For INT64/UINT64
 * the bound is the largest magnitude that double can represent; values
 * above that lose precision before reaching the parser, so the check is
 * grosso modo by design.
 */
static bool sdo_value_in_range(ecat_data_type_t dt, double v)
{
    if (isnan(v) || isinf(v))
        return false;
    switch (dt) {
    case ECAT_DTYPE_BOOL:   return v == 0.0 || v == 1.0;
    case ECAT_DTYPE_INT8:   return v >= INT8_MIN  && v <= INT8_MAX;
    case ECAT_DTYPE_UINT8:  return v >= 0         && v <= UINT8_MAX;
    case ECAT_DTYPE_INT16:  return v >= INT16_MIN && v <= INT16_MAX;
    case ECAT_DTYPE_UINT16: return v >= 0         && v <= UINT16_MAX;
    case ECAT_DTYPE_INT32:  return v >= INT32_MIN && v <= INT32_MAX;
    case ECAT_DTYPE_UINT32: return v >= 0         && v <= UINT32_MAX;
    case ECAT_DTYPE_INT64:  return v >= -9.223372036854776e18 && v <= 9.223372036854776e18;
    case ECAT_DTYPE_UINT64: return v >= 0 && v <= 1.844674407370955e19;
    case ECAT_DTYPE_REAL32: return v >= -FLT_MAX && v <= FLT_MAX;
    case ECAT_DTYPE_REAL64: return true;
    case ECAT_DTYPE_UNKNOWN:
    case ECAT_DTYPE_PAD:    return false;
    }
    return false;
}

/**
 * @brief Parse a single SDO configuration from JSON.
 *
 * Strict: malformed entries are rejected with a contextual error message
 * (index, subindex, type) so the operator can locate the bad SDO without
 * grepping warnings during bus open.
 *
 * @return ECAT_CONFIG_OK on success, error code otherwise.
 */
static int parse_sdo(const cJSON *sdo_json, ecat_sdo_config_t *sdo)
{
    if (sdo_json == NULL || sdo == NULL)
        return ECAT_CONFIG_ERR_INVALID;

    /* index: required, accepts hex (0x...) or decimal via base 0; range 0x0001..0xFFFF */
    const char *idx_str = get_string(sdo_json, "index", NULL);
    if (idx_str == NULL || idx_str[0] == '\0') {
        edog_log_error(g_config_logger, "SDO entry missing 'index'");
        return ECAT_CONFIG_ERR_MISSING;
    }
    char *endptr = NULL;
    unsigned long idx = strtoul(idx_str, &endptr, 0);
    if (endptr == idx_str || *endptr != '\0' || idx == 0 || idx > 0xFFFF) {
        edog_log_error(g_config_logger,
            "SDO 'index' invalid: '%s' (expected hex 0x0001..0xFFFF or decimal 1..65535)",
            idx_str);
        return ECAT_CONFIG_ERR_INVALID;
    }
    snprintf(sdo->index, sizeof(sdo->index), "0x%04lX", idx);

    /* subindex: optional, default 0 (single-entry SDOs).  When present must be 0..255 */
    const cJSON *si = cJSON_GetObjectItemCaseSensitive(sdo_json, "subindex");
    if (si == NULL) {
        sdo->subindex = 0;
    } else if (cJSON_IsNumber(si) && si->valueint >= 0 && si->valueint <= 255) {
        sdo->subindex = (uint8_t)si->valueint;
    } else {
        edog_log_error(g_config_logger,
            "SDO %s 'subindex' invalid (must be number 0..255)", sdo->index);
        return ECAT_CONFIG_ERR_INVALID;
    }

    /* data_type: required, must resolve to a known type (UNKNOWN/PAD reject) */
    const char *dtype_str = get_string(sdo_json, "data_type", "");
    sdo->parsed_type = ecat_parse_data_type(dtype_str);
    if (sdo->parsed_type == ECAT_DTYPE_UNKNOWN || sdo->parsed_type == ECAT_DTYPE_PAD) {
        edog_log_error(g_config_logger,
            "SDO %s:%d 'data_type' invalid: '%s' "
            "(use BOOL/INT8/UINT8/INT16/UINT16/INT32/UINT32/INT64/UINT64/REAL/LREAL)",
            sdo->index, sdo->subindex, dtype_str);
        return ECAT_CONFIG_ERR_INVALID;
    }

    /* R2: optional "value_bytes" overrides "value" and carries a raw byte string. Two
     * input shapes: a plain string ("UR20-4DI-P") is taken as its UTF-8 bytes, and a
     * "0x..." prefix is parsed as hex pairs. When present, the numeric value and the
     * range check are skipped: the payload wire-type is the bytes themselves. */
    sdo->value_bytes = NULL;
    sdo->value_bytes_len = 0;
    const cJSON *vbytes = cJSON_GetObjectItemCaseSensitive(sdo_json, "value_bytes");
    if (cJSON_IsString(vbytes) && vbytes->valuestring != NULL) {
        const char *s = vbytes->valuestring;
        size_t slen = strlen(s);
        if (slen >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            /* Hex-pair form "0xAABB..". Odd nibble counts after the prefix are rejected. */
            size_t hexlen = slen - 2;
            if (hexlen == 0 || (hexlen % 2) != 0) {
                edog_log_error(g_config_logger,
                    "SDO %s:%d 'value_bytes' hex payload must be an even nibble count",
                    sdo->index, sdo->subindex);
                return ECAT_CONFIG_ERR_INVALID;
            }
            size_t n = hexlen / 2;
            uint8_t *buf = (uint8_t *)calloc(n > 0 ? n : 1, 1);
            if (buf == NULL) return ECAT_CONFIG_ERR_MEMORY;
            for (size_t k = 0; k < n; k++) {
                char pair[3] = { s[2 + 2 * k], s[2 + 2 * k + 1], '\0' };
                char *endp = NULL;
                long v = strtol(pair, &endp, 16);
                if (endp != pair + 2 || v < 0 || v > 0xFF) {
                    free(buf);
                    edog_log_error(g_config_logger,
                        "SDO %s:%d 'value_bytes' non-hex byte at offset %zu",
                        sdo->index, sdo->subindex, k);
                    return ECAT_CONFIG_ERR_INVALID;
                }
                buf[k] = (uint8_t)v;
            }
            sdo->value_bytes = buf;
            sdo->value_bytes_len = n;
        } else {
            /* ASCII/UTF-8 string form. The trailing NUL is NOT included: a CANopen
             * VISIBLE_STRING written to 0x80n0:03 is the characters, not the terminator. */
            uint8_t *buf = (uint8_t *)calloc(slen > 0 ? slen : 1, 1);
            if (buf == NULL) return ECAT_CONFIG_ERR_MEMORY;
            memcpy(buf, s, slen);
            sdo->value_bytes = buf;
            sdo->value_bytes_len = slen;
        }
        /* Keep value_num at 0; the write path checks value_bytes first. */
        sdo->value = 0.0;
    } else {
        /* value: optional with default 0.  When present must fit the wire type. */
        sdo->value = get_numeric_value(sdo_json, "value", 0.0);
        if (!sdo_value_in_range(sdo->parsed_type, sdo->value)) {
            edog_log_error(g_config_logger,
                "SDO %s:%d 'value' %g out of range for type %s",
                sdo->index, sdo->subindex, sdo->value,
                ecat_data_type_to_string(sdo->parsed_type));
            return ECAT_CONFIG_ERR_INVALID;
        }
    }

    /* R2: optional "complete_access" flag defaults false. */
    sdo->complete_access = get_bool(sdo_json, "complete_access", false);

    safe_strcpy(sdo->name, get_string(sdo_json, "name", ""), sizeof(sdo->name));
    return ECAT_CONFIG_OK;
}

/**
 * @brief Parse a single channel from JSON
 */
static int parse_channel(const cJSON *ch_json, ecat_channel_t *channel)
{
    if (ch_json == NULL || channel == NULL) {
        return ECAT_CONFIG_ERR_INVALID;
    }

    channel->index = get_int(ch_json, "index", 0);
    safe_strcpy(channel->name, get_string(ch_json, "name", ""), sizeof(channel->name));
    safe_strcpy(channel->type, get_string(ch_json, "type", ""), sizeof(channel->type));
    channel->bit_length = (uint8_t)get_int(ch_json, "bit_length", 0);
    safe_strcpy(channel->pdo_index, get_string(ch_json, "pdo_index", ""), sizeof(channel->pdo_index));
    safe_strcpy(channel->pdo_entry_index, get_string(ch_json, "pdo_entry_index", ""), sizeof(channel->pdo_entry_index));
    channel->pdo_entry_subindex = (uint8_t)get_int(ch_json, "pdo_entry_subindex", 0);

    return ECAT_CONFIG_OK;
}

/**
 * @brief Parse a single slave configuration from JSON
 */
static int parse_slave(const cJSON *slave_json, ecat_slave_t *slave)
{
    if (slave_json == NULL || slave == NULL) {
        return ECAT_CONFIG_ERR_INVALID;
    }

    memset(slave, 0, sizeof(ecat_slave_t));

    slave->position = get_int(slave_json, "position", 0);
    safe_strcpy(slave->name, get_string(slave_json, "name", ""), sizeof(slave->name));
    safe_strcpy(slave->type, get_string(slave_json, "type", "coupler"), sizeof(slave->type));

    /* Convert hex string vendor_id, product_code, revision to uint32_t */
    slave->vendor_id = hex_to_uint32(get_string(slave_json, "vendor_id", "0x0"));
    slave->product_code = hex_to_uint32(get_string(slave_json, "product_code", "0x0"));
    slave->revision = hex_to_uint32(get_string(slave_json, "revision", "0x0"));

    /* Parse channels. Allocate sized exactly to the JSON array; the caller invokes
     * ecat_slave_destroy() on error, which releases every pointer we allocated here. */
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(slave_json, "channels");
    if (channels != NULL && cJSON_IsArray(channels)) {
        int n = cJSON_GetArraySize(channels);
        if (n > 0) {
            slave->channels = (ecat_channel_t *)calloc((size_t)n, sizeof(ecat_channel_t));
            if (slave->channels == NULL) {
                edog_log_error(g_config_logger,
                    "Slave '%s' position %d: out of memory allocating %d channels",
                    slave->name, slave->position, n);
                return ECAT_CONFIG_ERR_MEMORY;
            }
            slave->channel_capacity = n;
        }
        const cJSON *ch_json;
        cJSON_ArrayForEach(ch_json, channels) {
            if (parse_channel(ch_json, &slave->channels[slave->channel_count]) == ECAT_CONFIG_OK) {
                slave->channel_count++;
            }
        }
    }

    /* Parse SDO configurations. A malformed SDO aborts the slave entirely: a partial SDO
     * write set leaves the slave in an undefined state, so fail-fast at parse time forces
     * the operator to fix the JSON. */
    const cJSON *sdos = cJSON_GetObjectItemCaseSensitive(slave_json, "sdo_configurations");
    if (sdos != NULL && cJSON_IsArray(sdos)) {
        int n = cJSON_GetArraySize(sdos);
        if (n > 0) {
            slave->sdo_configs = (ecat_sdo_config_t *)calloc((size_t)n, sizeof(ecat_sdo_config_t));
            if (slave->sdo_configs == NULL) {
                edog_log_error(g_config_logger,
                    "Slave '%s' position %d: out of memory allocating %d SDOs",
                    slave->name, slave->position, n);
                return ECAT_CONFIG_ERR_MEMORY;
            }
            slave->sdo_capacity = n;
        }
        const cJSON *sdo_json;
        cJSON_ArrayForEach(sdo_json, sdos) {
            int prc = parse_sdo(sdo_json, &slave->sdo_configs[slave->sdo_count]);
            if (prc != ECAT_CONFIG_OK) {
                edog_log_error(g_config_logger,
                    "Slave '%s' position %d: SDO entry rejected (rc=%d) -- aborting slave parse",
                    slave->name, slave->position, prc);
                return prc;
            }
            slave->sdo_count++;
        }
    }

    /* Parse RxPDOs and TxPDOs */
    int prc;
    prc = parse_pdo_array(cJSON_GetObjectItemCaseSensitive(slave_json, "rx_pdos"),
                          &slave->rx_pdos, &slave->rx_pdo_count, &slave->rx_pdo_capacity);
    if (prc != ECAT_CONFIG_OK) {
        edog_log_error(g_config_logger,
            "Slave '%s' position %d: invalid RxPDO configuration (rc=%d)",
            slave->name, slave->position, prc);
        return prc;
    }
    prc = parse_pdo_array(cJSON_GetObjectItemCaseSensitive(slave_json, "tx_pdos"),
                          &slave->tx_pdos, &slave->tx_pdo_count, &slave->tx_pdo_capacity);
    if (prc != ECAT_CONFIG_OK) {
        edog_log_error(g_config_logger,
            "Slave '%s' position %d: invalid TxPDO configuration (rc=%d)",
            slave->name, slave->position, prc);
        return prc;
    }

    /* Parse per-slave configuration (defaults applied if "config" is absent) */
    slave->startup_checks.check_vendor_id = true;
    slave->startup_checks.check_product_code = true;
    slave->addressing.ethercat_address = 0;
    slave->timeouts.sdo_timeout_ms = 1000;
    slave->timeouts.init_to_preop_timeout_ms = 3000;
    slave->timeouts.safeop_to_op_timeout_ms = 10000;
    slave->watchdog.sm_watchdog_enabled = true;
    slave->watchdog.sm_watchdog_ms = 100;
    slave->watchdog.pdi_watchdog_enabled = false;
    slave->watchdog.pdi_watchdog_ms = 100;
    slave->dc.enabled = false;
    slave->dc.sync_unit_cycle_us = 0;
    slave->dc.sync0_enabled = false;
    slave->dc.sync0_cycle_us = 0;
    slave->dc.sync0_shift_us = 0;
    slave->dc.sync1_enabled = false;
    slave->dc.sync1_cycle_us = 0;
    slave->dc.sync1_shift_us = 0;
    slave->strict_sdo = true;

    const cJSON *cfg = cJSON_GetObjectItemCaseSensitive(slave_json, "config");
    if (cfg != NULL && cJSON_IsObject(cfg)) {
        slave->strict_sdo = get_bool(cfg, "strict_sdo", true);

        /* Startup checks */
        const cJSON *sc = cJSON_GetObjectItemCaseSensitive(cfg, "startup_checks");
        if (sc != NULL && cJSON_IsObject(sc)) {
            slave->startup_checks.check_vendor_id = get_bool(sc, "check_vendor_id", true);
            slave->startup_checks.check_product_code = get_bool(sc, "check_product_code", true);
        }

        /* Addressing */
        const cJSON *addr = cJSON_GetObjectItemCaseSensitive(cfg, "addressing");
        if (addr != NULL && cJSON_IsObject(addr)) {
            slave->addressing.ethercat_address = (uint16_t)get_int(addr, "ethercat_address", 0);
        }

        /* Timeouts (negative values fall back to defaults) */
        const cJSON *to = cJSON_GetObjectItemCaseSensitive(cfg, "timeouts");
        if (to != NULL && cJSON_IsObject(to)) {
            int val;
            val = get_int(to, "sdo_timeout_ms", 1000);
            if (val > 0) slave->timeouts.sdo_timeout_ms = val;
            val = get_int(to, "init_to_preop_timeout_ms", 3000);
            if (val > 0) slave->timeouts.init_to_preop_timeout_ms = val;
            val = get_int(to, "safeop_to_op_timeout_ms", 10000);
            if (val > 0) slave->timeouts.safeop_to_op_timeout_ms = val;
        }

        /* Watchdog (negative ms values fall back to defaults) */
        const cJSON *wd = cJSON_GetObjectItemCaseSensitive(cfg, "watchdog");
        if (wd != NULL && cJSON_IsObject(wd)) {
            int val;
            slave->watchdog.sm_watchdog_enabled = get_bool(wd, "sm_watchdog_enabled", true);
            val = get_int(wd, "sm_watchdog_ms", 100);
            if (val > 0) slave->watchdog.sm_watchdog_ms = val;
            slave->watchdog.pdi_watchdog_enabled = get_bool(wd, "pdi_watchdog_enabled", false);
            val = get_int(wd, "pdi_watchdog_ms", 100);
            if (val > 0) slave->watchdog.pdi_watchdog_ms = val;
        }

        /* Distributed Clocks */
        const cJSON *dc = cJSON_GetObjectItemCaseSensitive(cfg, "distributed_clocks");
        if (dc != NULL && cJSON_IsObject(dc)) {
            slave->dc.enabled = get_bool(dc, "enabled", false);
            slave->dc.sync_unit_cycle_us = get_int(dc, "sync_unit_cycle_us", 0);
            slave->dc.sync0_enabled = get_bool(dc, "sync0_enabled", false);
            slave->dc.sync0_cycle_us = get_int(dc, "sync0_cycle_us", 0);
            slave->dc.sync0_shift_us = get_int(dc, "sync0_shift_us", 0);
            slave->dc.sync1_enabled = get_bool(dc, "sync1_enabled", false);
            slave->dc.sync1_cycle_us = get_int(dc, "sync1_cycle_us", 0);
            slave->dc.sync1_shift_us = get_int(dc, "sync1_shift_us", 0);
        }
    }

    return ECAT_CONFIG_OK;
}

/**
 * @brief Parse the slaves array from JSON. Allocates config->slaves sized exactly to the
 * JSON array; the caller invokes ecat_config_destroy() on error.
 */
static int parse_slaves_section(const cJSON *slaves, ecat_config_t *config)
{
    if (slaves == NULL || !cJSON_IsArray(slaves)) {
        return ECAT_CONFIG_OK;
    }

    int n = cJSON_GetArraySize(slaves);
    if (n <= 0) {
        return ECAT_CONFIG_OK;
    }

    config->slaves = (ecat_slave_t *)calloc((size_t)n, sizeof(ecat_slave_t));
    if (config->slaves == NULL) {
        edog_log_error(g_config_logger, "out of memory allocating %d slaves", n);
        return ECAT_CONFIG_ERR_MEMORY;
    }
    config->slave_capacity = n;

    const cJSON *slave_json;
    cJSON_ArrayForEach(slave_json, slaves) {
        int prc = parse_slave(slave_json, &config->slaves[config->slave_count]);
        if (prc != ECAT_CONFIG_OK) {
            /* parse_slave already logged the specific reason; propagate. */
            return prc;
        }
        config->slave_count++;
    }

    return ECAT_CONFIG_OK;
}

/*
 * =============================================================================
 * Public API
 * =============================================================================
 */

void ecat_config_init_defaults(ecat_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(ecat_config_t));

    /* Master defaults */
    safe_strcpy(config->master.interface, "eth0", sizeof(config->master.interface));
    config->master.cycle_time_us = 1000;
    config->master.receive_timeout_us = 2000;
    config->master.watchdog_timeout_cycles = 3;
    safe_strcpy(config->master.log_level, "info", sizeof(config->master.log_level));
    config->master.task_priority = 90;
    config->master.safe_close = true;

    /* Diagnostics defaults */
    config->diagnostics.log_connections = true;
    config->diagnostics.log_data_access = false;
    config->diagnostics.log_errors = true;
    config->diagnostics.max_log_entries = 10000;
    config->diagnostics.status_update_interval_ms = 500;
}

int ecat_config_parse(const char *config_path, ecat_config_t *config)
{
    if (config_path == NULL || config == NULL) {
        return ECAT_CONFIG_ERR_INVALID;
    }

    /* Initialize with defaults */
    ecat_config_init_defaults(config);

    /* Read file contents */
    char *json_str = read_file(config_path);
    if (json_str == NULL) {
        return ECAT_CONFIG_ERR_FILE;
    }

    /* Parse JSON */
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (root == NULL) {
        return ECAT_CONFIG_ERR_PARSE;
    }

    /*
     * The JSON has array root format: [{ name, protocol, config }]
     * Extract the "config" object from the first element.
     */
    const cJSON *config_obj = NULL;

    if (cJSON_IsArray(root)) {
        const cJSON *first_entry = cJSON_GetArrayItem(root, 0);
        if (first_entry != NULL) {
            config_obj = cJSON_GetObjectItemCaseSensitive(first_entry, "config");
        }
    } else if (cJSON_IsObject(root)) {
        /* Also support a bare config object for flexibility */
        config_obj = cJSON_GetObjectItemCaseSensitive(root, "config");
        if (config_obj == NULL) {
            /* The root itself might be the config */
            config_obj = root;
        }
    }

    if (config_obj == NULL) {
        cJSON_Delete(root);
        return ECAT_CONFIG_ERR_PARSE;
    }

    /* Parse each section */
    parse_master_section(cJSON_GetObjectItemCaseSensitive(config_obj, "master"), &config->master);
    int srs = parse_slaves_section(cJSON_GetObjectItemCaseSensitive(config_obj, "slaves"), config);
    if (srs != ECAT_CONFIG_OK) {
        ecat_config_destroy(config);
        cJSON_Delete(root);
        return srs;
    }
    parse_diagnostics_section(cJSON_GetObjectItemCaseSensitive(config_obj, "diagnostics"), &config->diagnostics);

    cJSON_Delete(root);

    /* Validate the parsed configuration */
    int vrs = ecat_config_validate(config);
    if (vrs != ECAT_CONFIG_OK) {
        ecat_config_destroy(config);
    }
    return vrs;
}

int ecat_config_parse_all(const char *config_path,
                          ecat_master_instance_t *instances,
                          int max_masters,
                          int *out_count)
{
    if (config_path == NULL || instances == NULL || out_count == NULL || max_masters < 1) {
        return ECAT_CONFIG_ERR_INVALID;
    }

    *out_count = 0;

    /* Read file contents */
    char *json_str = read_file(config_path);
    if (json_str == NULL) {
        return ECAT_CONFIG_ERR_FILE;
    }

    /* Parse JSON */
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);

    if (root == NULL) {
        return ECAT_CONFIG_ERR_PARSE;
    }

    if (!cJSON_IsArray(root)) {
        /* Fall back to single-entry parse for bare config objects */
        ecat_config_init_defaults(&instances[0].config);
        const cJSON *config_obj = cJSON_GetObjectItemCaseSensitive(root, "config");
        if (config_obj == NULL) {
            config_obj = root;
        }
        const char *name = get_string(root, "name", "master");
        safe_strcpy(instances[0].name, name, sizeof(instances[0].name));
        parse_master_section(cJSON_GetObjectItemCaseSensitive(config_obj, "master"),
                             &instances[0].config.master);
        int srs = parse_slaves_section(cJSON_GetObjectItemCaseSensitive(config_obj, "slaves"),
                                       &instances[0].config);
        if (srs != ECAT_CONFIG_OK) {
            ecat_config_destroy(&instances[0].config);
            cJSON_Delete(root);
            return srs;
        }
        parse_diagnostics_section(cJSON_GetObjectItemCaseSensitive(config_obj, "diagnostics"),
                                  &instances[0].config.diagnostics);
        cJSON_Delete(root);
        int result = ecat_config_validate(&instances[0].config);
        if (result == ECAT_CONFIG_OK) {
            *out_count = 1;
        } else {
            ecat_config_destroy(&instances[0].config);
        }
        return result;
    }

    /* Iterate all entries in the array.  Loop runs to the end (not stops
     * at max_masters) so we can warn about entries that exceed the cap. */
    int count = 0;
    int array_size = cJSON_GetArraySize(root);

    for (int i = 0; i < array_size; i++) {
        const cJSON *entry = cJSON_GetArrayItem(root, i);
        if (entry == NULL) continue;

        /* Check protocol is ETHERCAT (case-insensitive) */
        const char *protocol = get_string(entry, "protocol", "");
        if (strcasecmp_local(protocol, "ETHERCAT") != 0) continue;

        const cJSON *config_obj = cJSON_GetObjectItemCaseSensitive(entry, "config");
        if (config_obj == NULL) continue;

        const char *name = get_string(entry, "name", "master");

        /* Refuse extra masters instead of running a subset of the configuration */
        if (count >= max_masters) {
            edog_log_error(g_config_logger, "entry[%d] '%s': more than %d masters", i, name,
                           max_masters);
            cJSON_Delete(root);
            *out_count = 0;
            return ECAT_CONFIG_ERR_INVALID;
        }

        /* Initialize this instance's config with defaults */
        ecat_config_init_defaults(&instances[count].config);

        /* Extract master name */
        safe_strcpy(instances[count].name, name, sizeof(instances[count].name));

        /* Parse configuration sections.  A SDO/slave-level error aborts
         * the whole config -- a partially loaded master would surprise
         * the operator at bus-up time. */
        parse_master_section(cJSON_GetObjectItemCaseSensitive(config_obj, "master"),
                             &instances[count].config.master);
        int srs = parse_slaves_section(cJSON_GetObjectItemCaseSensitive(config_obj, "slaves"),
                                       &instances[count].config);
        if (srs != ECAT_CONFIG_OK) {
            edog_log_error(g_config_logger,
                "entry[%d] '%s': slaves section failed (rc=%d) -- aborting parse",
                i, name, srs);
            ecat_config_destroy(&instances[count].config);
            for (int k = 0; k < count; k++)
                ecat_config_destroy(&instances[k].config);
            cJSON_Delete(root);
            *out_count = 0;
            return srs;
        }
        parse_diagnostics_section(cJSON_GetObjectItemCaseSensitive(config_obj, "diagnostics"),
                                  &instances[count].config.diagnostics);

        /* Validate this master's config */
        int result = ecat_config_validate(&instances[count].config);
        if (result == ECAT_CONFIG_OK) {
            count++;
        } else {
            edog_log_error(g_config_logger,
                "skipping entry[%d] '%s' (validation failed, error=%d)",
                i, name, result);
            ecat_config_destroy(&instances[count].config);
        }
    }

    cJSON_Delete(root);

    /* Refuse configs where two masters share the same network interface.
     * Per-iface NIC tuning state in ethercat_iface_state.c is
     * single-owner; two masters on the same iface produce corrupted
     * persistence on crash recovery. */
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(instances[i].config.master.interface,
                       instances[j].config.master.interface) == 0) {
                edog_log_error(g_config_logger,
                    "masters '%s' and '%s' share interface '%s' -- "
                    "not supported. Use a distinct interface per master.",
                    instances[i].name, instances[j].name,
                    instances[i].config.master.interface);
                for (int k = 0; k < count; k++)
                    ecat_config_destroy(&instances[k].config);
                *out_count = 0;
                return ECAT_CONFIG_ERR_INVALID;
            }
        }
    }

    *out_count = count;

    return (count > 0) ? ECAT_CONFIG_OK : ECAT_CONFIG_ERR_MISSING;
}

int ecat_config_validate(const ecat_config_t *config)
{
    if (config == NULL) {
        return ECAT_CONFIG_ERR_INVALID;
    }

    /* Validate master interface is not empty */
    if (config->master.interface[0] == '\0') {
        return ECAT_CONFIG_ERR_INVALID;
    }

    /* Validate cycle time */
    if (config->master.cycle_time_us < 1) {
        return ECAT_CONFIG_ERR_INVALID;
    }

    /* Validate receive timeout */
    if (config->master.receive_timeout_us < 1) {
        return ECAT_CONFIG_ERR_INVALID;
    }

    /* Validate slave positions are positive and unique */
    for (int i = 0; i < config->slave_count; i++) {
        const ecat_slave_t *slave = &config->slaves[i];

        if (slave->position < 1) {
            return ECAT_CONFIG_ERR_INVALID;
        }

        if (slave->vendor_id == 0) {
            return ECAT_CONFIG_ERR_INVALID;
        }

        if (slave->product_code == 0) {
            return ECAT_CONFIG_ERR_INVALID;
        }

        /* Check for duplicate positions */
        for (int j = i + 1; j < config->slave_count; j++) {
            if (slave->position == config->slaves[j].position) {
                return ECAT_CONFIG_ERR_INVALID;
            }
        }
    }

    return ECAT_CONFIG_OK;
}

/*
 * =============================================================================
 * State Machine and Data Type Helpers
 * =============================================================================
 */

const char *ecat_state_to_string(ecat_bus_state_t state)
{
    switch (state) {
    case ECAT_STATE_IDLE:          return "IDLE";
    case ECAT_STATE_SCANNING:      return "SCANNING";
    case ECAT_STATE_CONFIGURING:   return "CONFIGURING";
    case ECAT_STATE_TRANSITIONING: return "TRANSITIONING";
    case ECAT_STATE_OPERATIONAL:   return "OPERATIONAL";
    case ECAT_STATE_RECOVERING:    return "RECOVERING";
    case ECAT_STATE_ERROR:         return "ERROR";
    case ECAT_STATE_STOPPED:       return "STOPPED";
    }
    return "UNKNOWN";
}

int ecat_data_type_size(ecat_data_type_t dt)
{
    switch (dt) {
    case ECAT_DTYPE_BOOL:    return 1;
    case ECAT_DTYPE_INT8:    return 1;
    case ECAT_DTYPE_UINT8:   return 1;
    case ECAT_DTYPE_INT16:   return 2;
    case ECAT_DTYPE_UINT16:  return 2;
    case ECAT_DTYPE_INT32:   return 4;
    case ECAT_DTYPE_UINT32:  return 4;
    case ECAT_DTYPE_INT64:   return 8;
    case ECAT_DTYPE_UINT64:  return 8;
    case ECAT_DTYPE_REAL32:  return 4;
    case ECAT_DTYPE_REAL64:  return 8;
    case ECAT_DTYPE_UNKNOWN: return 0;
    case ECAT_DTYPE_PAD:     return 0;
    }
    return 0;
}

const char *ecat_data_type_to_string(ecat_data_type_t dt)
{
    switch (dt) {
    case ECAT_DTYPE_UNKNOWN: return "UNKNOWN";
    case ECAT_DTYPE_BOOL:    return "BOOL";
    case ECAT_DTYPE_INT8:    return "INT8";
    case ECAT_DTYPE_UINT8:   return "UINT8";
    case ECAT_DTYPE_INT16:   return "INT16";
    case ECAT_DTYPE_UINT16:  return "UINT16";
    case ECAT_DTYPE_INT32:   return "INT32";
    case ECAT_DTYPE_UINT32:  return "UINT32";
    case ECAT_DTYPE_INT64:   return "INT64";
    case ECAT_DTYPE_UINT64:  return "UINT64";
    case ECAT_DTYPE_REAL32:  return "REAL32";
    case ECAT_DTYPE_REAL64:  return "REAL64";
    case ECAT_DTYPE_PAD:     return "PAD";
    }
    return "UNKNOWN";
}

/*
 * =============================================================================
 * Destructors (RTOP-319 R1)
 * =============================================================================
 */

void ecat_pdo_destroy(ecat_pdo_t *pdo)
{
    if (pdo == NULL) return;
    free(pdo->entries);
    pdo->entries = NULL;
    pdo->entry_count = 0;
    pdo->entry_capacity = 0;
}

void ecat_slave_destroy(ecat_slave_t *slave)
{
    if (slave == NULL) return;

    free(slave->channels);
    slave->channels = NULL;
    slave->channel_count = 0;
    slave->channel_capacity = 0;

    if (slave->sdo_configs != NULL) {
        /* R2: each SDO config may own a heap byte-string payload */
        for (int i = 0; i < slave->sdo_count; i++) {
            free(slave->sdo_configs[i].value_bytes);
            slave->sdo_configs[i].value_bytes = NULL;
            slave->sdo_configs[i].value_bytes_len = 0;
        }
        free(slave->sdo_configs);
        slave->sdo_configs = NULL;
    }
    slave->sdo_count = 0;
    slave->sdo_capacity = 0;

    if (slave->rx_pdos != NULL) {
        for (int i = 0; i < slave->rx_pdo_count; i++)
            ecat_pdo_destroy(&slave->rx_pdos[i]);
        free(slave->rx_pdos);
        slave->rx_pdos = NULL;
    }
    slave->rx_pdo_count = 0;
    slave->rx_pdo_capacity = 0;

    if (slave->tx_pdos != NULL) {
        for (int i = 0; i < slave->tx_pdo_count; i++)
            ecat_pdo_destroy(&slave->tx_pdos[i]);
        free(slave->tx_pdos);
        slave->tx_pdos = NULL;
    }
    slave->tx_pdo_count = 0;
    slave->tx_pdo_capacity = 0;
}

void ecat_config_destroy(ecat_config_t *config)
{
    if (config == NULL) return;
    if (config->slaves != NULL) {
        for (int i = 0; i < config->slave_count; i++)
            ecat_slave_destroy(&config->slaves[i]);
        free(config->slaves);
        config->slaves = NULL;
    }
    config->slave_count = 0;
    config->slave_capacity = 0;
}

void ecat_layout_destroy(ecat_layout_t *layout)
{
    if (layout == NULL) return;
    free(layout->entries);
    layout->entries = NULL;
    layout->entry_count = 0;
    layout->entry_capacity = 0;
    layout->output_bytes = 0;
    layout->input_bytes = 0;
}

int ecat_layout_init(ecat_layout_t *layout, int capacity)
{
    if (layout == NULL || capacity < 0) return -1;
    ecat_layout_destroy(layout);
    if (capacity == 0) return 0;
    layout->entries = (ecat_layout_entry_t *)calloc((size_t)capacity, sizeof(ecat_layout_entry_t));
    if (layout->entries == NULL) {
        edog_log_error(g_config_logger, "layout: out of memory allocating %d entries", capacity);
        return -1;
    }
    layout->entry_capacity = capacity;
    return 0;
}

/*
 * Instance-level allocators. Both must be called before the bus thread starts, so no
 * concurrency guards are needed: the allocation ordering is enforced in master.c and
 * the two-consecutive-loads integration test is the gate for the lifecycle.
 */
int ecat_master_instance_alloc_iomap(ecat_master_instance_t *inst)
{
    if (inst == NULL) return -1;
    if (inst->iomap != NULL) {
        /* Already allocated; a reload path should have called _destroy first */
        edog_log_error(g_config_logger, "iomap already allocated at reload; invariant broken");
        return -1;
    }

    /* ecx_config_init populates slave identity but not Ibytes/Obytes -- those come from
     * ecx_config_map_group, which needs the buffer passed in. We therefore allocate the
     * initial size here and the caller fails the start if the mapping requires more.
     * The macro replaces the old struct-embedded ECAT_IOMAP_SIZE; raising it is a
     * one-constant tune, not a struct-layout change (RTOP-319 R1). */
    inst->iomap = (uint8_t *)calloc(1, ECAT_IOMAP_INITIAL_SIZE);
    if (inst->iomap == NULL) {
        edog_log_error(g_config_logger, "iomap: out of memory allocating %d bytes",
                       ECAT_IOMAP_INITIAL_SIZE);
        return -1;
    }
    inst->iomap_capacity = ECAT_IOMAP_INITIAL_SIZE;
    inst->iomap_used_size = 0;
    return 0;
}

int ecat_master_instance_alloc_snapshot(ecat_master_instance_t *inst)
{
    if (inst == NULL) return -1;
    if (inst->slaves_snapshot != NULL) {
        edog_log_error(g_config_logger, "slaves_snapshot already allocated at reload");
        return -1;
    }
    int n = inst->config.slave_count;
    if (n <= 0) {
        /* No slaves configured: still allocate one slot to keep readers simple */
        n = 1;
    }
    inst->slaves_snapshot =
        (ecat_slave_status_t *)calloc((size_t)n, sizeof(ecat_slave_status_t));
    if (inst->slaves_snapshot == NULL) {
        edog_log_error(g_config_logger, "slaves_snapshot: out of memory allocating %d slots", n);
        return -1;
    }
    inst->slaves_snapshot_capacity = n;
    inst->slaves_snapshot_count = 0;
    return 0;
}

void ecat_master_instance_destroy(ecat_master_instance_t *inst)
{
    if (inst == NULL) return;
    free(inst->iomap);
    inst->iomap = NULL;
    inst->iomap_capacity = 0;
    inst->iomap_used_size = 0;

    free(inst->slaves_snapshot);
    inst->slaves_snapshot = NULL;
    inst->slaves_snapshot_count = 0;
    inst->slaves_snapshot_capacity = 0;

    ecat_layout_destroy(&inst->layout);
    ecat_config_destroy(&inst->config);
}
