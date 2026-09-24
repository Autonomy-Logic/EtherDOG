// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file main.c
 * @brief EtherDOG entry point: argument parsing, signals, then the control loop.
 */

#include "bus.h"
#include "control.h"
#include "log.h"
#include "version.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/mman.h>
#endif

#if defined(__CYGWIN__) || defined(_WIN32)
#define DEFAULT_CONTROL "tcp:127.0.0.1:18444"
#else
#define DEFAULT_CONTROL "unix:/run/etherdog/etherdog.socket"
#endif

static volatile sig_atomic_t g_stop = 0;

static void on_terminate(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Bus threads sleep in clock_nanosleep; SIGUSR1 interrupts it so a stop lands promptly. */
static void on_wake(int sig)
{
    (void)sig;
}

static void usage(FILE *out)
{
    fprintf(out,
            "EtherDOG %s - EtherCAT master service\n"
            "\n"
            "Usage: etherdog [options]\n"
            "  --control <spec>     control socket: unix:<path> or tcp:127.0.0.1:<port>\n"
            "                       (default %s)\n"
            "  --token-file <path>  require clients to authenticate with this token\n"
            "                       (also read from $ETHERDOG_TOKEN)\n"
            "  --state-dir <dir>    data sockets and NIC recovery files (default /run/etherdog)\n"
            "  --config <file>      load a bus configuration at startup\n"
            "  --start              start the bus after loading --config\n"
            "  --log-level <level>  debug, info, warn or error (default info)\n"
            "  --log-socket <spec>  also stream log lines as JSON to unix:<path> or tcp:<ip>:<port>\n"
            "  --version            print the version and exit\n"
            "  --help               this text\n",
            EDOG_VERSION, DEFAULT_CONTROL);
}

static int read_token_file(const char *path, char *out, size_t size)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return -1;
    size_t n = fread(out, 1, size - 1, fp);
    fclose(fp);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        out[--n] = '\0';
    return n > 0 ? 0 : -1;
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = on_terminate;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = on_wake; /* no SA_RESTART: clock_nanosleep must return EINTR */
    sigaction(SIGUSR1, &sa, NULL);

    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

int main(int argc, char **argv)
{
    const char *control = DEFAULT_CONTROL;
    const char *token_file = NULL;
    const char *state_dir = "/run/etherdog";
    const char *config = NULL;
    const char *log_socket = NULL;
    int autostart = 0;
    edog_logger_t log;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("EtherDOG %s\n", EDOG_VERSION);
            return 0;
        } else if (strcmp(a, "--start") == 0) {
            autostart = 1;
        } else if (next != NULL && strcmp(a, "--control") == 0) {
            control = next;
            i++;
        } else if (next != NULL && strcmp(a, "--token-file") == 0) {
            token_file = next;
            i++;
        } else if (next != NULL && strcmp(a, "--state-dir") == 0) {
            state_dir = next;
            i++;
        } else if (next != NULL && strcmp(a, "--log-socket") == 0) {
            log_socket = next;
            i++;
        } else if (next != NULL && strcmp(a, "--config") == 0) {
            config = next;
            i++;
        } else if (next != NULL && strcmp(a, "--log-level") == 0) {
            edog_log_level_t level;
            if (!edog_log_parse_level(next, &level)) {
                fprintf(stderr, "unknown log level '%s'\n", next);
                return 2;
            }
            edog_log_set_level(level);
            i++;
        } else {
            fprintf(stderr, "unknown or incomplete option '%s'\n\n", a);
            usage(stderr);
            return 2;
        }
    }

    edog_logger_init(&log, "ETHERDOG");
    if (log_socket != NULL && edog_log_start_sink(log_socket) != 0) {
        fprintf(stderr, "invalid --log-socket '%s'\n", log_socket);
        return 2;
    }

    char token[256] = "";
    const char *env_token = getenv("ETHERDOG_TOKEN");
    if (token_file != NULL) {
        if (read_token_file(token_file, token, sizeof(token)) != 0) {
            edog_log_error(&log, "cannot read token file %s", token_file);
            return 1;
        }
    } else if (env_token != NULL && env_token[0] != '\0') {
        snprintf(token, sizeof(token), "%s", env_token);
    }

    if (mkdir(state_dir, 0700) != 0 && errno != EEXIST) {
        edog_log_error(&log, "cannot create state directory %s: %s", state_dir, strerror(errno));
        return 1;
    }

    install_signals();

#if defined(__linux__)
    /* Keep the bus thread's pages resident; a page fault mid-exchange is a missed cycle. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        edog_log_warn(&log, "mlockall failed (%s); running without locked memory",
                      strerror(errno));
#endif

    edog_log_info(&log, "EtherDOG %s starting (auth %s)", EDOG_VERSION,
                  token[0] ? "required" : "disabled");

    ecat_bus_init();
    ecat_bus_set_state_dir(state_dir);

    if (config != NULL) {
        char err[512];
        if (ecat_bus_configure(config, err, sizeof(err)) != 0) {
            edog_log_error(&log, "%s", err);
            return 1;
        }
        if (autostart && ecat_bus_start(err, sizeof(err)) < 0)
            edog_log_error(&log, "%s", err);
    }

    int rc = edog_control_run(control, token, &g_stop);

    edog_log_info(&log, "shutting down");
    ecat_bus_shutdown();
    edog_log_stop_sink();
    return rc == 0 ? 0 : 1;
}
