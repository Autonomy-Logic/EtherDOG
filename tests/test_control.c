// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file test_control.c
 * @brief Control server access rules: unix peers by uid, tcp only with a token.
 */

#include "control.h"
#include "unity.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t stop_flag;
static char spec[128];
static char path[108];
static const char *server_token;
static int server_rc;

static void *serve(void *arg)
{
    (void)arg;
    server_rc = edog_control_run(spec, server_token, NULL, 0, &stop_flag);
    return NULL;
}

void setUp(void)
{
    stop_flag = 0;
    server_token = NULL;
    snprintf(path, sizeof(path), "/tmp/edog-control-test-%ld.sock", (long)getpid());
    snprintf(spec, sizeof(spec), "unix:%s", path);
}

void tearDown(void)
{
    unlink(path);
}

static int connect_unix(void)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    for (int i = 0; i < 100; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;
        if (fd >= 0)
            close(fd);
        usleep(10000);
    }
    return -1;
}

static void request(int fd, const char *line, char *out, size_t size)
{
    TEST_ASSERT_TRUE(send(fd, line, strlen(line), 0) > 0);
    size_t used = 0;
    while (used + 1 < size) {
        ssize_t n = recv(fd, out + used, size - 1 - used, 0);
        if (n <= 0)
            break;
        used += (size_t)n;
        if (memchr(out, '\n', used) != NULL)
            break;
    }
    out[used] = '\0';
}

void test_unix_peer_with_own_uid_is_accepted(void)
{
    pthread_t t;
    pthread_create(&t, NULL, serve, NULL);
    int fd = connect_unix();
    TEST_ASSERT_TRUE(fd >= 0);

    char reply[256];
    request(fd, "{\"command\":\"hello\"}\n", reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"name\":\"EtherDOG\""));
    close(fd);

    stop_flag = 1;
    pthread_join(t, NULL);
    TEST_ASSERT_EQUAL_INT(0, server_rc);
}

void test_token_is_enforced_on_unix(void)
{
    server_token = "secret";
    pthread_t t;
    pthread_create(&t, NULL, serve, NULL);
    int fd = connect_unix();
    TEST_ASSERT_TRUE(fd >= 0);

    char reply[256];
    request(fd, "{\"command\":\"hello\",\"params\":{\"token\":\"wrong\"}}\n", reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "authentication failed"));
    close(fd);

    fd = connect_unix();
    request(fd, "{\"command\":\"hello\",\"params\":{\"token\":\"secret\"}}\n", reply, sizeof(reply));
    TEST_ASSERT_NOT_NULL(strstr(reply, "\"status\":\"success\""));
    close(fd);

    stop_flag = 1;
    pthread_join(t, NULL);
}

void test_tcp_without_token_is_refused(void)
{
    snprintf(spec, sizeof(spec), "tcp:127.0.0.1:18999");
    TEST_ASSERT_EQUAL_INT(-1, edog_control_run(spec, NULL, NULL, 0, &stop_flag));
    TEST_ASSERT_EQUAL_INT(-1, edog_control_run(spec, "", NULL, 0, &stop_flag));
}
