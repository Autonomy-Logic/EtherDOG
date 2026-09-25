// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file test_al_poll.c
 * @brief Unit tests for ecat_al_poll_healthy(), the cyclic AL status check
 */

#include "master.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_all_slaves_in_op_is_healthy(void)
{
    TEST_ASSERT_TRUE(ecat_al_poll_healthy(EC_STATE_OPERATIONAL, 3, 3));
}

void test_extra_responders_are_healthy(void)
{
    TEST_ASSERT_TRUE(ecat_al_poll_healthy(EC_STATE_OPERATIONAL, 4, 3));
}

void test_missing_slave_is_a_fault(void)
{
    TEST_ASSERT_FALSE(ecat_al_poll_healthy(EC_STATE_OPERATIONAL, 2, 3));
}

void test_error_flag_is_a_fault(void)
{
    TEST_ASSERT_FALSE(ecat_al_poll_healthy(EC_STATE_OPERATIONAL | EC_STATE_ERROR, 3, 3));
}

void test_mixed_states_are_a_fault(void)
{
    /* OP OR SAFE-OP */
    TEST_ASSERT_FALSE(ecat_al_poll_healthy(EC_STATE_OPERATIONAL | EC_STATE_SAFE_OP, 3, 3));
}

void test_safe_op_is_a_fault(void)
{
    TEST_ASSERT_FALSE(ecat_al_poll_healthy(EC_STATE_SAFE_OP, 3, 3));
}

void test_no_slaves_is_a_fault(void)
{
    TEST_ASSERT_FALSE(ecat_al_poll_healthy(EC_STATE_OPERATIONAL, 0, 0));
}

void test_high_status_bits_are_ignored(void)
{
    TEST_ASSERT_TRUE(ecat_al_poll_healthy(0x0100 | EC_STATE_OPERATIONAL, 1, 1));
}
