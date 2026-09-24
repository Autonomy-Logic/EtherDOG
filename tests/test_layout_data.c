// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file test_layout_data.c
 * @brief Layout walk and data session round trips over a fake mapped master.
 */

#include "config.h"
#include "data.h"
#include "dgram.h"
#include "etherdog_protocol.h"
#include "layout.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static ecat_master_instance_t inst;
static edog_logger_t logger;
static int client_fd = -1;
static char client_path[108];
static char server[160];
static const char *STATE_DIR = "/tmp";

static void add_entry(ecat_pdo_t *pdo, const char *index, uint8_t sub, uint8_t bits,
                      ecat_data_type_t type)
{
    ecat_pdo_entry_t *e = &pdo->entries[pdo->entry_count++];
    snprintf(e->index, sizeof(e->index), "%s", index);
    e->subindex = sub;
    e->bit_length = bits;
    e->parsed_type = type;
}

/* One slave: RxPDO = 1 bit + 7 bit pad (1 byte out), TxPDO = one 16-bit word (2 bytes in). */
void setUp(void)
{
    memset(&inst, 0, sizeof(inst));
    edog_logger_init(&logger, "TEST");
    snprintf(inst.name, sizeof(inst.name), "m0");
    inst.config.master.cycle_time_us = 1000;
    inst.config.slave_count = 1;
    ecat_slave_t *s = &inst.config.slaves[0];
    s->position = 1;
    snprintf(s->name, sizeof(s->name), "dev");
    s->rx_pdo_count = 1;
    snprintf(s->rx_pdos[0].index, sizeof(s->rx_pdos[0].index), "0x1600");
    add_entry(&s->rx_pdos[0], "0x7000", 1, 1, ECAT_DTYPE_BOOL);
    add_entry(&s->rx_pdos[0], "0x0000", 0, 7, ECAT_DTYPE_PAD);
    s->tx_pdo_count = 1;
    snprintf(s->tx_pdos[0].index, sizeof(s->tx_pdos[0].index), "0x1A00");
    add_entry(&s->tx_pdos[0], "0x6000", 1, 16, ECAT_DTYPE_UINT16);

    inst.soem_initialized = 1;
    inst.ecx_context.slavecount = 1;
    inst.ecx_context.grouplist[0].outputs = inst.iomap;
    inst.ecx_context.grouplist[0].inputs = inst.iomap + 1;
    inst.ecx_context.grouplist[0].Obytes = 1;
    inst.ecx_context.grouplist[0].Ibytes = 2;
    inst.ecx_context.slavelist[1].outputs = inst.iomap;
    inst.ecx_context.slavelist[1].inputs = inst.iomap + 1;

    TEST_ASSERT_EQUAL_INT(0, ecat_data_init(&inst));
}

void tearDown(void)
{
    ecat_data_destroy(&inst);
    edog_dgram_close(client_fd, client_path);
    client_fd = -1;
    client_path[0] = '\0';
}

/* The spec only picks the family; the client binds its own address. */
static void open_client(const char *family_spec, char *endpoint, size_t size)
{
    edog_dgram_peer_t family;
    char err[128];
    TEST_ASSERT_EQUAL_INT(0, edog_dgram_parse(family_spec, &family, err, sizeof(err)));
    client_fd = edog_dgram_open(&family, STATE_DIR, 99, client_path, sizeof(client_path),
                                endpoint, size, err, sizeof(err));
    TEST_ASSERT_TRUE_MESSAGE(client_fd >= 0, err);
}

static void send_outputs(const char *server, uint64_t session, uint32_t seq, uint8_t flags,
                         const uint8_t *payload, uint16_t len)
{
    uint8_t frame[EDOG_FRAME_HEADER_SIZE + 16];
    edog_frame_header_t h = { EDOG_PROTOCOL_VERSION, EDOG_FRAME_OUTPUTS, 0, flags, session, seq, len, 0 };
    edog_frame_encode_header(frame, &h);
    memcpy(frame + EDOG_FRAME_HEADER_SIZE, payload, len);

    edog_dgram_peer_t to;
    char err[128];
    TEST_ASSERT_EQUAL_INT(0, edog_dgram_parse(server, &to, err, sizeof(err)));
    TEST_ASSERT_TRUE(edog_dgram_send(client_fd, frame, EDOG_FRAME_HEADER_SIZE + len, &to) > 0);
}

static ssize_t recv_frame(uint8_t *buf, size_t size)
{
    edog_dgram_peer_t any = { .family = -1 };
    bool from_peer = false;
    for (int i = 0; i < 100; i++) {
        ssize_t n = edog_dgram_recv(client_fd, buf, size, &any, &from_peer);
        if (n >= 0)
            return n;
        usleep(1000);
    }
    return -1;
}

/* --- layout ------------------------------------------------------------------------------ */

void test_layout_publishes_entries_and_skips_padding(void)
{
    TEST_ASSERT_EQUAL_INT(0, ecat_layout_build(&inst, &logger));
    TEST_ASSERT_EQUAL_UINT32(1, inst.layout.output_bytes);
    TEST_ASSERT_EQUAL_UINT32(2, inst.layout.input_bytes);
    TEST_ASSERT_EQUAL_INT(2, inst.layout.entry_count);
    TEST_ASSERT_TRUE(inst.layout.entries[0].is_output);
    TEST_ASSERT_EQUAL_STRING("0x7000", inst.layout.entries[0].entry_index);
    TEST_ASSERT_EQUAL_UINT32(0, inst.layout.entries[0].bit_offset);
    TEST_ASSERT_FALSE(inst.layout.entries[1].is_output);
    TEST_ASSERT_EQUAL_UINT32(0, inst.layout.entries[1].bit_offset);
    TEST_ASSERT_EQUAL_UINT8(16, inst.layout.entries[1].bit_length);
}

void test_layout_fails_when_slave_missing(void)
{
    inst.ecx_context.slavecount = 0;
    TEST_ASSERT_EQUAL_INT(-1, ecat_layout_build(&inst, &logger));
}

/* --- data session ------------------------------------------------------------------------ */

/* Cygwin AF_UNIX datagrams carry no sender path, so the session cannot verify the peer */
#if defined(__CYGWIN__)
#define DEFAULT_UDP true
#else
#define DEFAULT_UDP false
#endif

static void round_trip(bool udp)
{
    TEST_ASSERT_EQUAL_INT(0, ecat_layout_build(&inst, &logger));
    char endpoint[160], err[256];
    uint64_t session = 0;
    open_client(udp ? "udp:127.0.0.1:1" : "unix:/unused", endpoint, sizeof(endpoint));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ecat_data_open(&inst, 0, endpoint, STATE_DIR, server,
                                                    sizeof(server), &session, err, sizeof(err)), err);
    TEST_ASSERT_TRUE(session != 0);

    inst.iomap[1] = 0x34;
    inst.iomap[2] = 0x12;
    ecat_data_publish_inputs(&inst, 0, 3, true, true);

    uint8_t buf[256];
    ssize_t n = recv_frame(buf, sizeof(buf));
    edog_frame_header_t h;
    TEST_ASSERT_TRUE(edog_frame_decode_header(buf, (size_t)n, &h));
    TEST_ASSERT_EQUAL_UINT8(EDOG_FRAME_INPUTS, h.kind);
    TEST_ASSERT_EQUAL_UINT64(session, h.session);
    TEST_ASSERT_EQUAL_UINT16(2, h.payload_len);
    TEST_ASSERT_EQUAL_UINT16(3, h.wkc);
    TEST_ASSERT_EQUAL_HEX8(EDOG_FLAG_VALID | EDOG_FLAG_WKC_OK, h.flags);
    TEST_ASSERT_EQUAL_HEX8(0x34, buf[EDOG_FRAME_HEADER_SIZE]);
    TEST_ASSERT_EQUAL_HEX8(0x12, buf[EDOG_FRAME_HEADER_SIZE + 1]);

    uint8_t out = 0x01;
    send_outputs(server, session, 1, EDOG_FLAG_VALID, &out, 1);
    usleep(2000);
    ecat_data_apply_outputs(&inst, 0);
    TEST_ASSERT_EQUAL_HEX8(0x01, inst.iomap[0]);
}

void test_data_round_trip_unix(void)
{
#if defined(__CYGWIN__)
    TEST_IGNORE_MESSAGE("AF_UNIX peer check unsupported on Cygwin");
#endif
    round_trip(false);
}

void test_data_round_trip_udp(void)
{
    round_trip(true);
}

void test_data_drops_wrong_session_and_old_sequence(void)
{
    round_trip(DEFAULT_UDP);

    uint8_t out = 0x00;
    send_outputs(server, inst.data.session + 1, 2, EDOG_FLAG_VALID, &out, 1);
    send_outputs(server, inst.data.session, 1, EDOG_FLAG_VALID, &out, 1);
    usleep(2000);
    ecat_data_apply_outputs(&inst, 0);
    TEST_ASSERT_EQUAL_HEX8(0x01, inst.iomap[0]);
    TEST_ASSERT_EQUAL_UINT64(2, atomic_load(&inst.data.frames_dropped));
}

void test_data_watchdog_zeroes_outputs(void)
{
    round_trip(DEFAULT_UDP);
    for (int i = 0; i < (ECAT_DATA_TIMEOUT_MS * 1000) / 1000 + 1; i++)
        ecat_data_apply_outputs(&inst, 0);
    TEST_ASSERT_EQUAL_HEX8(0x00, inst.iomap[0]);
    TEST_ASSERT_EQUAL_UINT64(1, atomic_load(&inst.data.watchdog_trips));
}

void test_data_rejects_non_loopback_udp(void)
{
    TEST_ASSERT_EQUAL_INT(0, ecat_layout_build(&inst, &logger));
    char server[160], err[256];
    uint64_t session;
    TEST_ASSERT_EQUAL_INT(-1, ecat_data_open(&inst, 0, "udp:10.0.0.1:5000", STATE_DIR, server,
                                             sizeof(server), &session, err, sizeof(err)));
    TEST_ASSERT_NOT_NULL(strstr(err, "loopback"));
}

void test_frame_header_rejects_bad_magic_and_length(void)
{
    uint8_t frame[EDOG_FRAME_HEADER_SIZE + 2] = { 0 };
    edog_frame_header_t h = { EDOG_PROTOCOL_VERSION, EDOG_FRAME_INPUTS, 0, 0, 7, 1, 2, 0 };
    edog_frame_encode_header(frame, &h);
    edog_frame_header_t out;
    TEST_ASSERT_TRUE(edog_frame_decode_header(frame, sizeof(frame), &out));
    TEST_ASSERT_FALSE(edog_frame_decode_header(frame, sizeof(frame) - 1, &out));
    frame[0] = 'X';
    TEST_ASSERT_FALSE(edog_frame_decode_header(frame, sizeof(frame), &out));
}
