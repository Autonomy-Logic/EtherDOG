// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file test_lifecycle.c
 * @brief Two-consecutive-loads lifecycle test for the heap-allocated configuration.
 *
 * Catches an array-to-pointer refactor that silently drops sizeof-based iteration or leaves a
 * dangling pointer after a reload. The test loads two configurations of different
 * shapes back-to-back and asserts the second load observes exactly the fields of the second
 * config, with no residue of the first.
 */

#include "config.h"
#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TMPFILE "/tmp/etherdog_test_lifecycle.json"

/*
 * A single master with @p slaves slaves, each slave with @p pdos_per_direction RxPDOs and
 * TxPDOs, each PDO with @p entries_per_pdo entries, each slave with @p sdos_per_slave SDO
 * configurations. Enough variability to produce distinct per-slave heap allocations between
 * the two loads.
 */
static void write_config(int slaves, int pdos_per_direction, int entries_per_pdo,
                         int sdos_per_slave)
{
    FILE *fp = fopen(TMPFILE, "w");
    TEST_ASSERT_NOT_NULL(fp);

    fprintf(fp, "[{\"name\":\"m0\",\"protocol\":\"ETHERCAT\",\"config\":{");
    fprintf(fp, "\"master\":{\"interface\":\"eth0\",\"cycle_time_us\":1000,"
                "\"receive_timeout_us\":2000},");
    fprintf(fp, "\"slaves\":[");
    for (int s = 0; s < slaves; s++) {
        if (s > 0) fprintf(fp, ",");
        fprintf(fp, "{\"position\":%d,\"name\":\"s%d\",\"type\":\"digital_input\","
                    "\"vendor_id\":\"0x2\",\"product_code\":\"0x1\",\"revision\":\"0x1\"",
                    s + 1, s);
        fprintf(fp, ",\"sdo_configurations\":[");
        for (int k = 0; k < sdos_per_slave; k++) {
            if (k > 0) fprintf(fp, ",");
            fprintf(fp, "{\"index\":\"0x%04X\",\"subindex\":%d,\"data_type\":\"UINT16\","
                        "\"value\":%d,\"name\":\"sdo%d\"}", 0x8000 + k, k, 100 + k, k);
        }
        fprintf(fp, "]");
        fprintf(fp, ",\"rx_pdos\":[");
        for (int p = 0; p < pdos_per_direction; p++) {
            if (p > 0) fprintf(fp, ",");
            fprintf(fp, "{\"index\":\"0x%04X\",\"name\":\"r%d\",\"entries\":[", 0x1600 + p, p);
            for (int e = 0; e < entries_per_pdo; e++) {
                if (e > 0) fprintf(fp, ",");
                fprintf(fp, "{\"index\":\"0x%04X\",\"subindex\":%d,\"bit_length\":8,"
                            "\"data_type\":\"UINT8\",\"name\":\"re%d\"}",
                            0x7000 + e, e, e);
            }
            fprintf(fp, "]}");
        }
        fprintf(fp, "],");
        fprintf(fp, "\"tx_pdos\":[");
        for (int p = 0; p < pdos_per_direction; p++) {
            if (p > 0) fprintf(fp, ",");
            fprintf(fp, "{\"index\":\"0x%04X\",\"name\":\"t%d\",\"entries\":[", 0x1A00 + p, p);
            for (int e = 0; e < entries_per_pdo; e++) {
                if (e > 0) fprintf(fp, ",");
                fprintf(fp, "{\"index\":\"0x%04X\",\"subindex\":%d,\"bit_length\":8,"
                            "\"data_type\":\"UINT8\",\"name\":\"te%d\"}",
                            0x6000 + e, e, e);
            }
            fprintf(fp, "]}");
        }
        fprintf(fp, "]}");
    }
    fprintf(fp, "]}}]");
    fclose(fp);
}

void setUp(void) {}

void tearDown(void)
{
    unlink(TMPFILE);
}

/* Load A, destroy, load B of a different shape, destroy, repeat. The failure mode the test
 * catches: a stale pointer from load A reaching load B's bookkeeping, or load B observing
 * residual counts from load A. Address-sanitizer in the build amplifies these to crashes;
 * at a minimum the asserts here fail. */
void test_lifecycle_TwoConsecutiveLoads_LeavesNoResidue(void)
{
    ecat_master_instance_t *g = calloc(ECAT_MAX_MASTERS, sizeof(ecat_master_instance_t));
    TEST_ASSERT_NOT_NULL(g);

    for (int round = 0; round < 10; round++) {
        /* Load A: 2 slaves, 3 PDOs/dir, 4 entries/PDO, 2 SDOs/slave. */
        write_config(2, 3, 4, 2);
        int count = 0;
        TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK,
            ecat_config_parse_all(TMPFILE, g, ECAT_MAX_MASTERS, &count));
        TEST_ASSERT_EQUAL_INT(1, count);
        TEST_ASSERT_EQUAL_INT(2, g[0].config.slave_count);
        TEST_ASSERT_EQUAL_INT(3, g[0].config.slaves[0].rx_pdo_count);
        TEST_ASSERT_EQUAL_INT(4, g[0].config.slaves[0].rx_pdos[0].entry_count);
        TEST_ASSERT_EQUAL_INT(2, g[0].config.slaves[0].sdo_count);
        TEST_ASSERT_NOT_NULL(g[0].config.slaves);
        TEST_ASSERT_NOT_NULL(g[0].config.slaves[0].rx_pdos);
        TEST_ASSERT_NOT_NULL(g[0].config.slaves[0].rx_pdos[0].entries);

        /* Destroy A. */
        ecat_master_instance_destroy(&g[0]);
        TEST_ASSERT_NULL(g[0].config.slaves);
        TEST_ASSERT_EQUAL_INT(0, g[0].config.slave_count);
        TEST_ASSERT_EQUAL_INT(0, g[0].config.slave_capacity);

        /* Load B: a different shape -- 1 slave, 5 PDOs/dir, 2 entries/PDO, 7 SDOs. The
         * change forces different allocation sizes, so a reuse-after-free from A would
         * most likely show up here as a crash or asserting the wrong count. */
        write_config(1, 5, 2, 7);
        count = 0;
        TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK,
            ecat_config_parse_all(TMPFILE, g, ECAT_MAX_MASTERS, &count));
        TEST_ASSERT_EQUAL_INT(1, count);
        TEST_ASSERT_EQUAL_INT(1, g[0].config.slave_count);
        TEST_ASSERT_EQUAL_INT(5, g[0].config.slaves[0].rx_pdo_count);
        TEST_ASSERT_EQUAL_INT(2, g[0].config.slaves[0].rx_pdos[0].entry_count);
        TEST_ASSERT_EQUAL_INT(7, g[0].config.slaves[0].sdo_count);

        ecat_master_instance_destroy(&g[0]);
        TEST_ASSERT_NULL(g[0].config.slaves);
    }

    free(g);
}

/* Confirm the destructor is idempotent and null-safe, so a double-free or NULL-free in a
 * reload path does not crash. */
void test_lifecycle_DestroyIsIdempotentAndNullSafe(void)
{
    ecat_config_destroy(NULL);
    ecat_pdo_destroy(NULL);
    ecat_slave_destroy(NULL);
    ecat_layout_destroy(NULL);
    ecat_master_instance_destroy(NULL);

    ecat_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    ecat_config_destroy(&cfg);  /* zero-initialised: safe */
    ecat_config_destroy(&cfg);  /* idempotent: safe */
}

/* The iomap allocation sizes itself from the parsed config. A larger project (more slaves,
 * more PDO bits) must produce a strictly larger iomap than a small one; a tiny project must
 * still fall above the hard floor. Re-allocating across loads of different shapes exercises
 * the alloc_iomap → destroy → alloc_iomap path. */
void test_lifecycle_IomapIsSizedFromParsedConfig(void)
{
    ecat_master_instance_t *g = calloc(ECAT_MAX_MASTERS, sizeof(ecat_master_instance_t));
    TEST_ASSERT_NOT_NULL(g);

    /* Small project. */
    write_config(1, 1, 1, 0);
    int count = 0;
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK,
        ecat_config_parse_all(TMPFILE, g, ECAT_MAX_MASTERS, &count));
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, ecat_master_instance_alloc_iomap(&g[0]));
    size_t small_bytes = g[0].iomap_capacity;
    TEST_ASSERT_TRUE(small_bytes >= ECAT_IOMAP_MIN_BYTES);
    ecat_master_instance_destroy(&g[0]);
    TEST_ASSERT_NULL(g[0].iomap);
    TEST_ASSERT_EQUAL_INT(0, (int)g[0].iomap_capacity);

    /* Larger project. 8 slaves × 4 PDOs/dir × 8 entries/PDO × 8 bits = many bytes. */
    write_config(8, 4, 8, 0);
    count = 0;
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK,
        ecat_config_parse_all(TMPFILE, g, ECAT_MAX_MASTERS, &count));
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, ecat_master_instance_alloc_iomap(&g[0]));
    size_t big_bytes = g[0].iomap_capacity;
    TEST_ASSERT_TRUE(big_bytes > small_bytes);
    /* Expected bytes: 8 slaves × 2 directions × 4 PDOs × 8 entries × 8 bits = 4096 bits →
     * 512 bytes + 8 × 32 slack = 768.  The capacity must at least cover that. */
    TEST_ASSERT_TRUE(big_bytes >= 512 + 8 * ECAT_IOMAP_SLACK_PER_SLAVE);
    ecat_master_instance_destroy(&g[0]);
    TEST_ASSERT_NULL(g[0].iomap);

    free(g);
}

/* Confirm a parse failure leaves the config in a destroy-safe state and never leaks the
 * partially populated allocations. The config is "bad" because its first SDO has an
 * unknown data type, which the parser rejects after allocating the SDO buffer. */
void test_lifecycle_ParseFailureDoesNotLeak(void)
{
    FILE *fp = fopen(TMPFILE, "w");
    TEST_ASSERT_NOT_NULL(fp);
    fprintf(fp, "[{\"name\":\"m0\",\"protocol\":\"ETHERCAT\",\"config\":{"
                "\"master\":{\"interface\":\"eth0\",\"cycle_time_us\":1000,"
                "\"receive_timeout_us\":2000},"
                "\"slaves\":[{\"position\":1,\"name\":\"s0\","
                "\"vendor_id\":\"0x2\",\"product_code\":\"0x1\",\"revision\":\"0x1\","
                "\"sdo_configurations\":[{\"index\":\"0x8000\",\"subindex\":0,"
                "\"data_type\":\"MYSTERY\",\"value\":1}]}]}}]");
    fclose(fp);

    ecat_master_instance_t *g = calloc(ECAT_MAX_MASTERS, sizeof(ecat_master_instance_t));
    TEST_ASSERT_NOT_NULL(g);
    int count = 0;
    int rc = ecat_config_parse_all(TMPFILE, g, ECAT_MAX_MASTERS, &count);
    TEST_ASSERT_NOT_EQUAL(ECAT_CONFIG_OK, rc);

    /* Parser internally called ecat_config_destroy on the failing instance, so g[0] is
     * now in a destroy-safe state. Double-destroying it must not crash. */
    ecat_master_instance_destroy(&g[0]);
    free(g);
}

/* The iomap only grows before mapping: a larger request replaces the buffer zeroed, a smaller
 * one keeps it. */
void test_lifecycle_GrowIomapOnlyGrows(void)
{
    ecat_master_instance_t *inst = calloc(1, sizeof(*inst));
    TEST_ASSERT_NOT_NULL(inst);
    TEST_ASSERT_EQUAL_INT(0, ecat_master_instance_grow_iomap(inst, 64));
    TEST_ASSERT_EQUAL_INT(64, (int)inst->iomap_capacity);
    inst->iomap[63] = 0xAA;
    uint8_t *before = inst->iomap;
    TEST_ASSERT_EQUAL_INT(0, ecat_master_instance_grow_iomap(inst, 32));
    TEST_ASSERT_EQUAL_PTR(before, inst->iomap);
    TEST_ASSERT_EQUAL_INT(0, ecat_master_instance_grow_iomap(inst, 100000));
    TEST_ASSERT_EQUAL_INT(100000, (int)inst->iomap_capacity);
    TEST_ASSERT_EQUAL_HEX8(0, inst->iomap[63]);
    ecat_master_instance_destroy(inst);
    TEST_ASSERT_NULL(inst->iomap);
    free(inst);
}

/* Start, stop, start: the per-start buffers are allocated, released and allocated again. */
void test_lifecycle_SnapshotAndIomapSurviveRepeatedStarts(void)
{
    ecat_master_instance_t *inst = calloc(1, sizeof(*inst));
    TEST_ASSERT_NOT_NULL(inst);
    write_config(3, 1, 2, 0);
    int count = 0;
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, ecat_config_parse_all(TMPFILE, inst, 1, &count));
    for (int start = 0; start < 3; start++) {
        TEST_ASSERT_EQUAL_INT(0, ecat_master_instance_alloc_iomap(inst));
        TEST_ASSERT_EQUAL_INT(0, ecat_master_instance_alloc_snapshot(inst));
        TEST_ASSERT_EQUAL_INT(3, inst->slaves_snapshot_capacity);
        /* A second allocation without a release is an invariant break, not a leak. */
        TEST_ASSERT_EQUAL_INT(-1, ecat_master_instance_alloc_snapshot(inst));
        free(inst->iomap);
        inst->iomap = NULL;
        inst->iomap_capacity = 0;
        free(inst->slaves_snapshot);
        inst->slaves_snapshot = NULL;
        inst->slaves_snapshot_capacity = 0;
    }
    ecat_master_instance_destroy(inst);
    free(inst);
    unlink(TMPFILE);
}
