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

#define DEFAULT_CONTROL "unix:/run/etherdog/etherdog.socket"
#define MAX_ALLOW_UIDS 8

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
            "  --allow-uid <uid>    also accept unix socket clients running as this uid\n"
            "                       (EtherDOG's own uid and root are always accepted)\n"
            "  --token-stdin        read a token from the first line of stdin and require\n"
            "                       clients to send it in 'hello' (also $ETHERDOG_TOKEN);\n"
            "                       mandatory for a tcp control socket\n"
            "  --state-dir <dir>    data sockets and NIC recovery files (default /run/etherdog)\n"
            "  --config <file>      load a bus configuration at startup\n"
            "  --start              start the bus after loading --config\n"
            "  --log-level <level>  debug, info, warn or error (default info)\n"
            "  --log-socket <spec>  also stream log lines as JSON to unix:<path> or tcp:<ip>:<port>\n"
            "  --version            print the version and exit\n"
            "  --help               this text\n",
            EDOG_VERSION, DEFAULT_CONTROL);
}

static int read_token_line(FILE *in, char *out, size_t size)
{
    if (fgets(out, (int)size, in) == NULL)
        return -1;
    size_t n = strlen(out);
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
    int token_stdin = 0;
    uid_t allow_uids[MAX_ALLOW_UIDS];
    int allow_count = 0;
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
        } else if (strcmp(a, "--token-stdin") == 0) {
            token_stdin = 1;
        } else if (next != NULL && strcmp(a, "--allow-uid") == 0) {
            char *end = NULL;
            long uid = strtol(next, &end, 10);
            if (end == next || *end != '\0' || uid < 0 || allow_count >= MAX_ALLOW_UIDS) {
                fprintf(stderr, "invalid --allow-uid '%s' (at most %d)\n", next, MAX_ALLOW_UIDS);
                return 2;
            }
            allow_uids[allow_count++] = (uid_t)uid;
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
    if (token_stdin) {
        if (read_token_line(stdin, token, sizeof(token)) != 0) {
            edog_log_error(&log, "--token-stdin: no token on stdin");
            return 2;
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

    edog_log_info(&log, "EtherDOG %s starting (token %s)", EDOG_VERSION,
                  token[0] ? "required" : "not required");

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

    int rc = edog_control_run(control, token, allow_uids, allow_count, &g_stop);

    edog_log_info(&log, "shutting down");
    ecat_bus_shutdown();
    edog_log_stop_sink();
    return rc == 0 ? 0 : 1;
}
