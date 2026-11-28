// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file test_dc_times.c
 * @brief Unit tests for the Distributed Clocks times: the nanosecond keys of
 *        `distributed_clocks` and ecat_dc_times(), the values handed to SOEM.
 *
 * Some devices only accept a SYNC0 cycle that is not a whole number of microseconds
 * (Beckhoff AX8000: ESI OpMode DcSync62500, SYNC0 = 62500 ns), so the cycle and shift
 * can also be given in nanoseconds. The microsecond keys keep their meaning.
 */

#include "config.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

static const char *TMPFILE = "test_dc_times_tmp.json";

/* Static buffers because ecat_master_instance_t is multi-MB. */
static ecat_master_instance_t g_instances[ECAT_MAX_MASTERS];

void setUp(void)
{
    memset(g_instances, 0, sizeof(g_instances));
}

void tearDown(void)
{
    for (int i = 0; i < ECAT_MAX_MASTERS; i++)
        ecat_config_destroy(&g_instances[i].config);
    remove(TMPFILE);
}

/* A one-slave bus at a 1000 us cycle with the given `distributed_clocks` object. */
static int parse_dc(const char *dc_json)
{
    FILE *fp = fopen(TMPFILE, "w");
    TEST_ASSERT_NOT_NULL(fp);
    fprintf(fp,
        "[{\"name\": \"m\", \"protocol\": \"ETHERCAT\", \"config\": {"
        "  \"master\": {\"interface\": \"eth0\", \"cycle_time_us\": 1000},"
        "  \"slaves\": [{\"position\": 1, \"name\": \"Drive\","
        "    \"vendor_id\": \"0x00000002\", \"product_code\": \"0x200e6012\", \"revision\": \"0x006a0064\","
        "    \"config\": {\"distributed_clocks\": %s},"
        "    \"channels\": [], \"sdo_configurations\": [], \"rx_pdos\": [], \"tx_pdos\": []}],"
        "  \"diagnostics\": {}}}]",
        dc_json);
    fclose(fp);
    int count = 0;
    return ecat_config_parse_all(TMPFILE, g_instances, ECAT_MAX_MASTERS, &count);
}

static ecat_dc_times_t times_of_first_slave(void)
{
    ecat_dc_times_t t;
    ecat_dc_times(&g_instances[0].config.slaves[0].dc, g_instances[0].config.master.cycle_time_us, &t);
    return t;
}

/* =====================================================================
 *  The microsecond keys: unchanged
 * ===================================================================== */

void test_dc_us_Sync0_ShouldUseMicroseconds(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, parse_dc(
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync0_cycle_us\": 500, \"sync0_shift_us\": 20}"));
    ecat_dc_times_t t = times_of_first_slave();
    TEST_ASSERT_EQUAL_INT(ECAT_DC_SYNC0, t.mode);
    TEST_ASSERT_EQUAL_UINT32(500000u, t.cycle0_ns);
    TEST_ASSERT_EQUAL_INT32(20000, t.shift_ns);
}

void test_dc_us_ZeroCycles_ShouldFallBackToTheBusCycle(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, parse_dc(
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync1_enabled\": true,"
        " \"sync0_cycle_us\": 0, \"sync1_cycle_us\": 0}"));
    ecat_dc_times_t t = times_of_first_slave();
    TEST_ASSERT_EQUAL_INT(ECAT_DC_SYNC01, t.mode);
    TEST_ASSERT_EQUAL_UINT32(1000000u, t.cycle0_ns);
    TEST_ASSERT_EQUAL_UINT32(1000000u, t.cycle1_ns);
}

void test_dc_us_SyncUnitCycle_ShouldReplaceTheBusCycle(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, parse_dc(
        "{\"enabled\": true, \"sync_unit_cycle_us\": 2000, \"sync0_enabled\": true}"));
    TEST_ASSERT_EQUAL_UINT32(2000000u, times_of_first_slave().cycle0_ns);
}

void test_dc_Disabled_OrNoSync0_ShouldBeOff(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, parse_dc("{\"enabled\": false, \"sync0_enabled\": true}"));
    TEST_ASSERT_EQUAL_INT(ECAT_DC_OFF, times_of_first_slave().mode);
    tearDown();
    setUp();
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, parse_dc("{\"enabled\": true, \"sync1_enabled\": true}"));
    TEST_ASSERT_EQUAL_INT(ECAT_DC_OFF, times_of_first_slave().mode);
}

/* =====================================================================
 *  The nanosecond keys
 * ===================================================================== */

void test_dc_ns_Ax8000_ShouldGiveSync0At62500ns(void)
{
    /* ESI OpMode DcSync62500: SYNC0 62500 ns, SYNC1 once per 1 ms bus cycle. SOEM's model
     * (ecx_dcsync01: TrueCyclTime = (CyclTime1 / CyclTime0 + 1) * CyclTime0) gives SYNC1 at
     * the SYNC0 after CyclTime1, so 15 * 62500 = 937500 ns makes it every 16th SYNC0 = 1 ms */
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, parse_dc(
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync1_enabled\": true,"
        " \"sync0_cycle_us\": 62, \"sync0_cycle_ns\": 62500, \"sync1_cycle_ns\": 937500}"));
    ecat_dc_times_t t = times_of_first_slave();
    TEST_ASSERT_EQUAL_INT(ECAT_DC_SYNC01, t.mode);
    TEST_ASSERT_EQUAL_UINT32(62500u, t.cycle0_ns);
    TEST_ASSERT_EQUAL_UINT32(937500u, t.cycle1_ns);
    TEST_ASSERT_EQUAL_INT32(0, t.shift_ns);
}

void test_dc_ns_Sync1Zero_ShouldFireWithSync0(void)
{
    /* With sync1_cycle_ns, 0 means "together with SYNC0" (SOEM), not "the bus cycle" */
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, parse_dc(
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync1_enabled\": true, \"sync1_cycle_ns\": 0}"));
    ecat_dc_times_t t = times_of_first_slave();
    TEST_ASSERT_EQUAL_UINT32(1000000u, t.cycle0_ns);
    TEST_ASSERT_EQUAL_UINT32(0u, t.cycle1_ns);
}

void test_dc_ns_NegativeShift_ShouldBeKept(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, parse_dc(
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync0_shift_ns\": -1500}"));
    TEST_ASSERT_EQUAL_INT32(-1500, times_of_first_slave().shift_ns);
}

void test_dc_ns_OverMicroseconds_ShouldWin(void)
{
    TEST_ASSERT_EQUAL_INT(ECAT_CONFIG_OK, parse_dc(
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync0_cycle_us\": 250, \"sync0_cycle_ns\": 125000,"
        " \"sync0_shift_us\": 10, \"sync0_shift_ns\": 2500}"));
    ecat_dc_times_t t = times_of_first_slave();
    TEST_ASSERT_EQUAL_UINT32(125000u, t.cycle0_ns);
    TEST_ASSERT_EQUAL_INT32(2500, t.shift_ns);
}

/* A wrong nanosecond value is an error, not a silent default: a drive that gets the
 * wrong SYNC0 refuses SAFE-OP (AL 0x0030) far from the JSON that caused it. */
void test_dc_ns_BadValues_ShouldReject(void)
{
    const char *bad[] = {
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync0_cycle_ns\": 0}",
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync0_cycle_ns\": -62500}",
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync0_cycle_ns\": 62500.5}",
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync0_cycle_ns\": \"62500\"}",
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync0_cycle_ns\": 4294967296}",
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync1_cycle_ns\": -1}",
        "{\"enabled\": true, \"sync0_enabled\": true, \"sync0_shift_ns\": 2147483648}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(ECAT_CONFIG_OK, parse_dc(bad[i]), bad[i]);
        tearDown();
        setUp();
    }
}
