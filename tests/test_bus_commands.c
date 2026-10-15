// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file test_bus_commands.c
 * @brief Argument handling of scan-modules and diagnostics-slave with no bus configured.
 */

#include "bus.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

static char *reply;

void setUp(void)
{
    ecat_bus_init();
    reply = NULL;
}

void tearDown(void)
{
    free(reply);
}

static const char *run(const char *json)
{
    free(reply);
    reply = ecat_bus_command(json);
    TEST_ASSERT_NOT_NULL(reply);
    return reply;
}

void test_scan_modules_by_master_needs_a_configured_master(void)
{
    TEST_ASSERT_NOT_NULL(strstr(run("{\"command\":\"scan-modules\",\"params\":{\"master\":0,\"slave\":1}}"),
                                "master index out of range"));
}

void test_scan_modules_refuses_slaves_outside_1_to_65535(void)
{
    const char *bad[] = {
        "{\"command\":\"scan-modules\",\"params\":{\"interface\":\"eth0\",\"slave\":0}}",
        "{\"command\":\"scan-modules\",\"params\":{\"interface\":\"eth0\",\"slave\":65536}}",
        "{\"command\":\"scan-modules\",\"params\":{\"interface\":\"eth0\",\"slave\":1.5}}",
        "{\"command\":\"scan-modules\",\"params\":{\"interface\":\"eth0\"}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(run(bad[i]), "must be an integer in 1..65535"), bad[i]);
}

void test_scan_modules_by_interface_validates_the_name(void)
{
    TEST_ASSERT_NOT_NULL(strstr(run("{\"command\":\"scan-modules\",\"params\":{\"interface\":\"eth0; rm\",\"slave\":1}}"),
                                "invalid interface name"));
    TEST_ASSERT_NOT_NULL(strstr(run("{\"command\":\"scan-modules\",\"params\":{\"interface\":5,\"slave\":1}}"),
                                "'interface' must be a string"));
}

void test_scan_modules_by_interface_reports_an_interface_it_cannot_open(void)
{
    const char *r = run("{\"command\":\"scan-modules\",\"params\":{\"interface\":\"edogtest9\",\"slave\":1}}");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(r, "\"status\":\"error\""), r);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(r, "edogtest9"), r);
}

void test_diagnostics_slave_needs_a_configured_master(void)
{
    TEST_ASSERT_NOT_NULL(strstr(run("{\"command\":\"diagnostics-slave\",\"params\":{\"master\":0,\"slave\":1}}"),
                                "master index out of range"));
    TEST_ASSERT_NOT_NULL(strstr(run("{\"command\":\"diagnostics-slave\",\"params\":{\"master\":0}}"),
                                "must be numbers"));
}
