// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file test_al_poll.c
 * @brief Unit tests for the cyclic AL status poll: the health check and the reply collection
 */

#include "master.h"
#include "unity.h"

#include <string.h>

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

#ifdef __linux__
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

/* --- ecat_master_al_poll_collect against SOEM's buffers, fed through a socket pair --- */

static ecat_master_instance_t g_inst;
static int g_peer = -1;

static void setup_port(void)
{
    memset(&g_inst, 0, sizeof(g_inst));
    ecx_portt *port = &g_inst.ecx_context.port;
    int sv[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_DGRAM, 0, sv));
    port->sockhandle = sv[0];
    g_peer = sv[1];
    port->stack.sock = &port->sockhandle;
    port->stack.txbuf = &port->txbuf;
    port->stack.txbuflength = &port->txbuflength;
    port->stack.tempbuf = &port->tempinbuf;
    port->stack.rxbuf = &port->rxbuf;
    port->stack.rxbufstat = &port->rxbufstat;
    port->stack.rxsa = &port->rxsa;
    pthread_mutex_init(&port->rx_mutex, NULL);
}

static void teardown_port(void)
{
    ecx_portt *port = &g_inst.ecx_context.port;
    close(port->sockhandle);
    close(g_peer);
    pthread_mutex_destroy(&port->rx_mutex);
}

/* A BRD reply to the AL status register as it comes off the wire; returns its length. */
static int build_reply(uint8 idx, uint16 al_status, uint16 wkc, uint8 *frame)
{
    ecx_portt *port = &g_inst.ecx_context.port;
    memset(frame, 0, EC_BUFSIZE);
    ((ec_etherheadert *)frame)->etype = htons(ETH_P_ECAT);
    uint16 zero = 0;
    ecx_setupdatagram(port, frame, EC_CMD_BRD, idx, 0, ECT_REG_ALSTAT, sizeof(zero), &zero);
    uint8 *data = frame + ETH_HEADERSIZE + EC_HEADERSIZE;
    data[0] = (uint8)(al_status & 0xFF);
    data[1] = (uint8)(al_status >> 8);
    data[2] = (uint8)(wkc & 0xFF);
    data[3] = (uint8)(wkc >> 8);
    return port->txbuflength[idx];
}
#endif

void test_collect_reads_a_reply_the_process_data_receive_stored(void)
{
#ifdef __linux__
    setup_port();
    ecx_portt *port = &g_inst.ecx_context.port;
    uint8 frame[EC_BUFSIZE];
    int len = build_reply(5, EC_STATE_OPERATIONAL, 3, frame);
    memcpy(port->rxbuf[5], frame + ETH_HEADERSIZE, (size_t)(len - ETH_HEADERSIZE));
    port->rxbufstat[5] = EC_BUF_RCVD;

    uint16_t status = 0;
    int wkc = 0;
    TEST_ASSERT_TRUE(ecat_master_al_poll_collect(&g_inst, 5, &status, &wkc));
    TEST_ASSERT_EQUAL_HEX16(EC_STATE_OPERATIONAL, status);
    TEST_ASSERT_EQUAL_INT(3, wkc);
    TEST_ASSERT_EQUAL_INT(EC_BUF_EMPTY, port->rxbufstat[5]);
    teardown_port();
#else
    TEST_IGNORE_MESSAGE("Linux SOEM port only");
#endif
}

void test_collect_reads_a_reply_still_on_the_socket(void)
{
#ifdef __linux__
    setup_port();
    ecx_portt *port = &g_inst.ecx_context.port;
    uint8 frame[EC_BUFSIZE];
    int len = build_reply(7, EC_STATE_SAFE_OP | EC_STATE_ERROR, 2, frame);
    port->rxbufstat[7] = EC_BUF_TX;
    TEST_ASSERT_EQUAL_INT(len, (int)send(g_peer, frame, (size_t)len, 0));

    uint16_t status = 0;
    int wkc = 0;
    TEST_ASSERT_TRUE(ecat_master_al_poll_collect(&g_inst, 7, &status, &wkc));
    TEST_ASSERT_EQUAL_HEX16(EC_STATE_SAFE_OP | EC_STATE_ERROR, status);
    TEST_ASSERT_EQUAL_INT(2, wkc);
    TEST_ASSERT_EQUAL_INT(EC_BUF_EMPTY, port->rxbufstat[7]);
    teardown_port();
#else
    TEST_IGNORE_MESSAGE("Linux SOEM port only");
#endif
}

void test_collect_releases_the_index_when_no_reply_came(void)
{
#ifdef __linux__
    setup_port();
    ecx_portt *port = &g_inst.ecx_context.port;
    port->rxbufstat[9] = EC_BUF_TX;

    uint16_t status = 0xBEEF;
    int wkc = -7;
    TEST_ASSERT_FALSE(ecat_master_al_poll_collect(&g_inst, 9, &status, &wkc));
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, status);
    TEST_ASSERT_EQUAL_INT(-7, wkc);
    TEST_ASSERT_EQUAL_INT(EC_BUF_EMPTY, port->rxbufstat[9]);
    teardown_port();
#else
    TEST_IGNORE_MESSAGE("Linux SOEM port only");
#endif
}
