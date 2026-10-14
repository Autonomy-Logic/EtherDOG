// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file test_iface_promisc.c
 * @brief Unit tests for ecat_iface_request_promisc() and the patched SOEM
 *        ecx_setupnic() that no longer sets interface flags.
 *
 * Guard and failure paths run unprivileged. The raw-socket tests need
 * CAP_NET_RAW and are ignored without it; with CAP_NET_ADMIN available they
 * drop it from the effective set so ecx_init() is proven to open without it.
 */

#include "iface_state.h"
#include "log.h"
#include "soem/soem.h"
#include "unity.h"

#include <string.h>

#ifdef __linux__
#include <arpa/inet.h>
#include <linux/capability.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#define LOOPBACK "lo"
#define FLAG_PROMISC 0x100

static edog_logger_t logger;
static uint64_t first_log_id;
static int restore_net_admin;

static int capget_now(struct __user_cap_header_struct *hdr, struct __user_cap_data_struct *data)
{
    memset(hdr, 0, sizeof(*hdr));
    memset(data, 0, sizeof(struct __user_cap_data_struct) * _LINUX_CAPABILITY_U32S_3);
    hdr->version = _LINUX_CAPABILITY_VERSION_3;
    return (int)syscall(SYS_capget, hdr, data);
}

/* Set or clear CAP_NET_ADMIN in the effective set; it stays permitted, so it can come back. */
static int set_net_admin_effective(int on)
{
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
    unsigned int bit = 1u << CAP_NET_ADMIN;

    if (capget_now(&hdr, data) != 0)
        return -1;
    if (on)
        data[0].effective |= bit;
    else
        data[0].effective &= ~bit;
    return (int)syscall(SYS_capset, &hdr, data);
}

static int has_net_admin_effective(void)
{
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];

    if (capget_now(&hdr, data) != 0)
        return 0;
    return (data[0].effective & (1u << CAP_NET_ADMIN)) != 0;
}

static int warnings_since_setup(const char *needle)
{
    edog_log_entry_t entries[16];
    int n = edog_log_query(first_log_id, EDOG_LOG_WARN, entries, 16);
    int found = 0;

    for (int i = 0; i < n; i++) {
        if (strstr(entries[i].message, needle))
            found++;
    }
    return found;
}

static int raw_socket_allowed(void)
{
    int fd = socket(PF_PACKET, SOCK_RAW, htons(0x88A4));
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static int read_flags(const char *iface, unsigned int *flags)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s/flags", iface);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fscanf(f, "%x", flags) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

static int udp_socket_ctx(ecx_contextt *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->port.sockhandle = socket(AF_INET, SOCK_DGRAM, 0);
    return ctx->port.sockhandle;
}

void setUp(void)
{
    edog_logger_init(&logger, "test");
    first_log_id = edog_log_next_id();
    restore_net_admin = 0;
}

void tearDown(void)
{
    if (restore_net_admin)
        set_net_admin_effective(1);
}

#else

void setUp(void) {}
void tearDown(void) {}

#endif

/* ---- ecat_iface_request_promisc: guards ---- */

void test_request_promisc_NullContext_ShouldWarnAndFail(void)
{
#ifdef __linux__
    TEST_ASSERT_EQUAL_INT(-1, ecat_iface_request_promisc(NULL, LOOPBACK, &logger));
    TEST_ASSERT_EQUAL_INT(1, warnings_since_setup("lo: promiscuous mode not requested"));
#else
    TEST_IGNORE_MESSAGE("Linux SOEM port only");
#endif
}

void test_request_promisc_ClosedSocket_ShouldWarnAndFail(void)
{
#ifdef __linux__
    ecx_contextt ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.port.sockhandle = -1;

    TEST_ASSERT_EQUAL_INT(-1, ecat_iface_request_promisc(&ctx, LOOPBACK, &logger));
    TEST_ASSERT_EQUAL_INT(1, warnings_since_setup("lo: promiscuous mode not requested"));
#else
    TEST_IGNORE_MESSAGE("Linux SOEM port only");
#endif
}

void test_request_promisc_NullIface_ShouldWarnAndFail(void)
{
#ifdef __linux__
    ecx_contextt ctx;
    TEST_ASSERT_TRUE(udp_socket_ctx(&ctx) >= 0);

    TEST_ASSERT_EQUAL_INT(-1, ecat_iface_request_promisc(&ctx, NULL, &logger));
    TEST_ASSERT_EQUAL_INT(1, warnings_since_setup("(null): promiscuous mode not requested"));
    close(ctx.port.sockhandle);
#else
    TEST_IGNORE_MESSAGE("Linux SOEM port only");
#endif
}

void test_request_promisc_UnknownIface_ShouldWarnAndFail(void)
{
#ifdef __linux__
    ecx_contextt ctx;
    TEST_ASSERT_TRUE(udp_socket_ctx(&ctx) >= 0);

    TEST_ASSERT_EQUAL_INT(-1, ecat_iface_request_promisc(&ctx, "nosuchif0", &logger));
    TEST_ASSERT_EQUAL_INT(1, warnings_since_setup("nosuchif0: promiscuous mode not requested"));
    close(ctx.port.sockhandle);
#else
    TEST_IGNORE_MESSAGE("Linux SOEM port only");
#endif
}

/* ---- ecat_iface_request_promisc: a refused request is not fatal ---- */

void test_request_promisc_RefusedRequest_ShouldWarnWithReason(void)
{
#ifdef __linux__
    ecx_contextt ctx;
    TEST_ASSERT_TRUE(udp_socket_ctx(&ctx) >= 0);

    /* A UDP socket refuses SOL_PACKET options, standing in for any setsockopt failure. */
    TEST_ASSERT_EQUAL_INT(-1, ecat_iface_request_promisc(&ctx, LOOPBACK, &logger));
    TEST_ASSERT_EQUAL_INT(1, warnings_since_setup("lo: promiscuous mode not enabled"));
    close(ctx.port.sockhandle);
#else
    TEST_IGNORE_MESSAGE("Linux SOEM port only");
#endif
}

/* ---- Raw socket: the kernel drops the request on close ---- */

void test_request_promisc_RawSocket_ShouldEnableUntilClose(void)
{
#ifdef __linux__
    unsigned int before = 0, during = 0, after = 0;

    if (!raw_socket_allowed())
        TEST_IGNORE_MESSAGE("needs CAP_NET_RAW");
    TEST_ASSERT_EQUAL_INT(0, read_flags(LOOPBACK, &before));
    if (before & FLAG_PROMISC)
        TEST_IGNORE_MESSAGE("lo is already promiscuous");

    ecx_contextt ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(1, ecx_init(&ctx, LOOPBACK));

    TEST_ASSERT_EQUAL_INT(0, ecat_iface_request_promisc(&ctx, LOOPBACK, &logger));
    TEST_ASSERT_EQUAL_INT(0, read_flags(LOOPBACK, &during));
    TEST_ASSERT_TRUE(during & FLAG_PROMISC);

    ecx_close(&ctx);
    TEST_ASSERT_EQUAL_INT(0, read_flags(LOOPBACK, &after));
    TEST_ASSERT_FALSE(after & FLAG_PROMISC);
#else
    TEST_IGNORE_MESSAGE("Linux SOEM port only");
#endif
}

/* ---- Patched SOEM ecx_setupnic ---- */

void test_ecx_init_WithoutNetAdmin_ShouldOpen(void)
{
#ifdef __linux__
    if (!raw_socket_allowed())
        TEST_IGNORE_MESSAGE("needs CAP_NET_RAW");
    if (has_net_admin_effective()) {
        TEST_ASSERT_EQUAL_INT(0, set_net_admin_effective(0));
        restore_net_admin = 1;
    }
    TEST_ASSERT_FALSE(has_net_admin_effective());

    ecx_contextt ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(1, ecx_init(&ctx, LOOPBACK));
    ecx_close(&ctx);
#else
    TEST_IGNORE_MESSAGE("Linux SOEM port only");
#endif
}

void test_ecx_init_UnknownIface_ShouldFail(void)
{
#ifdef __linux__
    if (!raw_socket_allowed())
        TEST_IGNORE_MESSAGE("needs CAP_NET_RAW");

    ecx_contextt ctx;
    memset(&ctx, 0, sizeof(ctx));
    TEST_ASSERT_EQUAL_INT(0, ecx_init(&ctx, "nosuchif0"));
    ecx_close(&ctx);
#else
    TEST_IGNORE_MESSAGE("Linux SOEM port only");
#endif
}

/* ---- Non-Linux: promiscuous mode is left to the SOEM port ---- */

void test_request_promisc_NonLinux_ShouldBeNoop(void)
{
#ifdef __linux__
    TEST_IGNORE_MESSAGE("non-Linux only");
#else
    TEST_ASSERT_EQUAL_INT(0, ecat_iface_request_promisc(NULL, "eth0", NULL));
#endif
}
