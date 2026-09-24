// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file test_layout_data.c
 * @brief Layout walk and data session round trips over a fake mapped master.
 */

#include "config.h"
#include "data.h"
#include "etherdog_protocol.h"
#include "layout.h"
#include "unity.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static ecat_master_instance_t inst;
static edog_logger_t logger;
static int client_fd = -1;
static char client_path[108];
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
    if (client_fd >= 0)
        close(client_fd);
    client_fd = -1;
    if (client_path[0] != '\0')
        unlink(client_path);
    client_path[0] = '\0';
}

static void open_unix_client(char *endpoint, size_t size)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(client_path, sizeof(client_path), "/tmp/edog-test-client-%ld.sock", (long)getpid());
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", client_path);
    unlink(client_path);
    client_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    TEST_ASSERT_TRUE(client_fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, bind(client_fd, (struct sockaddr *)&addr, sizeof(addr)));
    snprintf(endpoint, size, "unix:%s", client_path);
}

static void open_udp_client(char *endpoint, size_t size)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT_TRUE(client_fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, bind(client_fd, (struct sockaddr *)&addr, sizeof(addr)));
    TEST_ASSERT_EQUAL_INT(0, getsockname(client_fd, (struct sockaddr *)&addr, &len));
    snprintf(endpoint, size, "udp:127.0.0.1:%u", (unsigned)ntohs(addr.sin_port));
}

static void send_outputs(const char *server, uint64_t session, uint32_t seq, uint8_t flags,
                         const uint8_t *payload, uint16_t len)
{
    uint8_t frame[EDOG_FRAME_HEADER_SIZE + 16];
    edog_frame_header_t h = { EDOG_PROTOCOL_VERSION, EDOG_FRAME_OUTPUTS, 0, flags, session, seq, len, 0 };
    edog_frame_encode_header(frame, &h);
    memcpy(frame + EDOG_FRAME_HEADER_SIZE, payload, len);

    struct sockaddr_storage to;
    socklen_t to_len;
    memset(&to, 0, sizeof(to));
    if (strncmp(server, "unix:", 5) == 0) {
        struct sockaddr_un *un = (struct sockaddr_un *)&to;
        un->sun_family = AF_UNIX;
        snprintf(un->sun_path, sizeof(un->sun_path), "%s", server + 5);
        to_len = sizeof(*un);
    } else {
        struct sockaddr_in *in = (struct sockaddr_in *)&to;
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        in->sin_port = htons((uint16_t)atoi(strrchr(server, ':') + 1));
        to_len = sizeof(*in);
    }
    TEST_ASSERT_TRUE(sendto(client_fd, frame, EDOG_FRAME_HEADER_SIZE + len, 0,
                            (struct sockaddr *)&to, to_len) > 0);
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

static void round_trip(bool udp)
{
    TEST_ASSERT_EQUAL_INT(0, ecat_layout_build(&inst, &logger));
    char endpoint[160], server[160], err[256];
    uint64_t session = 0;
    if (udp)
        open_udp_client(endpoint, sizeof(endpoint));
    else
        open_unix_client(endpoint, sizeof(endpoint));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ecat_data_open(&inst, 0, endpoint, STATE_DIR, server,
                                                    sizeof(server), &session, err, sizeof(err)), err);
    TEST_ASSERT_TRUE(session != 0);

    inst.iomap[1] = 0x34;
    inst.iomap[2] = 0x12;
    ecat_data_publish_inputs(&inst, 0, 3, true, true);

    uint8_t buf[256];
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
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
    round_trip(false);
}

void test_data_round_trip_udp(void)
{
    round_trip(true);
}

void test_data_drops_wrong_session_and_old_sequence(void)
{
    round_trip(false);
    char server[160];
    snprintf(server, sizeof(server), "unix:%s", inst.data.local_path);

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
    round_trip(false);
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
