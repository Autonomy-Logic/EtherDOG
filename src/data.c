// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Autonomy®

/**
 * @file data.c
 * @brief Cyclic data session over a non-blocking datagram socket (AF_UNIX or loopback UDP).
 */

#include "data.h"
#include "layout.h"
#include "etherdog_protocol.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int ecat_data_init(ecat_master_instance_t *inst)
{
    ecat_data_session_t *d = &inst->data;
    memset(d, 0, sizeof(*d));
    d->fd = -1;
    return pthread_mutex_init(&d->lock, NULL) == 0 ? 0 : -1;
}

void ecat_data_destroy(ecat_master_instance_t *inst)
{
    ecat_data_close(inst);
    ecat_data_free_buffers(inst);
    pthread_mutex_destroy(&inst->data.lock);
}

int ecat_data_alloc_buffers(ecat_master_instance_t *inst, char *err, size_t err_size)
{
    ecat_data_session_t *d = &inst->data;
    uint32_t out_bytes = inst->layout.output_bytes;
    uint32_t in_bytes = inst->layout.input_bytes;
    if (out_bytes > EDOG_FRAME_MAX_PAYLOAD || in_bytes > EDOG_FRAME_MAX_PAYLOAD) {
        snprintf(err, err_size, "process image (out=%u in=%u bytes) exceeds the %d-byte frame payload",
                 out_bytes, in_bytes, EDOG_FRAME_MAX_PAYLOAD);
        return -1;
    }
    /* One spare byte: an oversized datagram then fails the length check instead of fitting. */
    size_t rx_size = (size_t)EDOG_FRAME_HEADER_SIZE + out_bytes + 1;
    size_t tx_size = (size_t)EDOG_FRAME_HEADER_SIZE + in_bytes;
    uint8_t *rx = calloc(1, rx_size);
    uint8_t *latest = calloc(1, out_bytes > 0 ? out_bytes : 1);
    uint8_t *tx = calloc(1, tx_size);
    if (rx == NULL || latest == NULL || tx == NULL) {
        free(rx);
        free(latest);
        free(tx);
        snprintf(err, err_size, "out of memory allocating data frame buffers (%zu+%zu bytes)", rx_size,
                 tx_size);
        return -1;
    }

    pthread_mutex_lock(&d->lock);
    free(d->rx_buf);
    free(d->rx_latest);
    free(d->tx_buf);
    d->rx_buf = rx;
    d->rx_buf_size = rx_size;
    d->rx_latest = latest;
    d->tx_buf = tx;
    d->tx_buf_size = tx_size;
    pthread_mutex_unlock(&d->lock);
    return 0;
}

void ecat_data_free_buffers(ecat_master_instance_t *inst)
{
    ecat_data_session_t *d = &inst->data;
    pthread_mutex_lock(&d->lock);
    free(d->rx_buf);
    free(d->rx_latest);
    free(d->tx_buf);
    d->rx_buf = NULL;
    d->rx_latest = NULL;
    d->tx_buf = NULL;
    d->rx_buf_size = 0;
    d->tx_buf_size = 0;
    pthread_mutex_unlock(&d->lock);
}

static uint64_t random_session(void)
{
    uint64_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, &v, sizeof(v));
        close(fd);
        if (n == (ssize_t)sizeof(v) && v != 0)
            return v;
    }
    /* Fallback: not secret, but still distinct per session. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    v = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)getpid() << 16);
    return v ? v : 1;
}

/* Caller holds d->lock. */
static void close_locked(ecat_data_session_t *d)
{
    edog_dgram_close(d->fd, d->local_path);
    d->fd = -1;
    d->local_path[0] = '\0';
    d->open = false;
    d->session = 0;
    d->rx_seen = false;
}

void ecat_data_close(ecat_master_instance_t *inst)
{
    ecat_data_session_t *d = &inst->data;
    pthread_mutex_lock(&d->lock);
    close_locked(d);
    pthread_mutex_unlock(&d->lock);
}

int ecat_data_open(ecat_master_instance_t *inst, int master_index, const char *client_endpoint,
                   const char *state_dir, char *endpoint_out, size_t endpoint_size,
                   uint64_t *session_out, char *err, size_t err_size)
{
    edog_dgram_peer_t peer;
    if (client_endpoint == NULL || edog_dgram_parse(client_endpoint, &peer, err, err_size) != 0)
        return -1;

    ecat_data_session_t *d = &inst->data;
    pthread_mutex_lock(&d->lock);
    close_locked(d);
    if (d->rx_buf == NULL || d->tx_buf == NULL) {
        pthread_mutex_unlock(&d->lock);
        snprintf(err, err_size, "data buffers not allocated; start the bus first");
        return -1;
    }

    int fd = edog_dgram_open(&peer, state_dir, master_index, d->local_path,
                             sizeof(d->local_path), endpoint_out, endpoint_size, err, err_size);
    if (fd < 0) {
        pthread_mutex_unlock(&d->lock);
        return -1;
    }

    d->fd = fd;
    d->peer = peer;
    d->session = random_session();
    d->tx_seq = 0;
    d->rx_last_seq = 0;
    d->rx_seen = false;
    d->cycles_since_output = 0;
    d->outputs_zeroed = false;
    d->open = true;
    *session_out = d->session;
    pthread_mutex_unlock(&d->lock);
    return 0;
}

static uint32_t timeout_cycles(const ecat_master_instance_t *inst)
{
    int cycle_us = inst->config.master.cycle_time_us > 0 ? inst->config.master.cycle_time_us : 1000;
    uint32_t cycles = (uint32_t)((ECAT_DATA_TIMEOUT_MS * 1000) / cycle_us);
    return cycles < 3 ? 3 : cycles;
}

void ecat_data_apply_outputs(ecat_master_instance_t *inst, int master_index)
{
    ecat_data_session_t *d = &inst->data;
    uint8_t *region = ecat_layout_output_region(inst);
    uint32_t out_bytes = inst->layout.output_bytes;
    if (region == NULL)
        return;

    if (pthread_mutex_trylock(&d->lock) != 0)
        return; /* control thread is reconfiguring; keep last outputs one cycle */

    if (!d->open) {
        if (!d->outputs_zeroed) {
            memset(region, 0, out_bytes);
            d->outputs_zeroed = true;
        }
        pthread_mutex_unlock(&d->lock);
        return;
    }

    uint8_t *buf = d->rx_buf;
    uint8_t *latest = d->rx_latest;
    bool have_new = false;
    bool latest_valid = false;

    for (;;) {
        bool from_peer = false;
        ssize_t n = edog_dgram_recv(d->fd, buf, d->rx_buf_size, &d->peer, &from_peer);
        if (n < 0)
            break; /* EAGAIN: drained */

        edog_frame_header_t h;
        if (!edog_frame_decode_header(buf, (size_t)n, &h) || h.kind != EDOG_FRAME_OUTPUTS ||
            h.master != (uint8_t)master_index || h.session != d->session ||
            h.payload_len != out_bytes || !from_peer ||
            (d->rx_seen && !edog_seq_newer(h.sequence, d->rx_last_seq))) {
            atomic_fetch_add_explicit(&d->frames_dropped, 1, memory_order_relaxed);
            continue;
        }
        d->rx_seen = true;
        d->rx_last_seq = h.sequence;
        latest_valid = (h.flags & EDOG_FLAG_VALID) != 0;
        if (latest_valid)
            memcpy(latest, buf + EDOG_FRAME_HEADER_SIZE, out_bytes);
        have_new = true;
        atomic_fetch_add_explicit(&d->frames_rx, 1, memory_order_relaxed);
    }

    if (have_new) {
        if (latest_valid) {
            memcpy(region, latest, out_bytes);
            d->outputs_zeroed = false;
        } else {
            memset(region, 0, out_bytes);
            d->outputs_zeroed = true;
        }
        d->cycles_since_output = 0;
    } else if (++d->cycles_since_output > timeout_cycles(inst) && !d->outputs_zeroed) {
        memset(region, 0, out_bytes);
        d->outputs_zeroed = true;
        atomic_fetch_add_explicit(&d->watchdog_trips, 1, memory_order_relaxed);
    }

    pthread_mutex_unlock(&d->lock);
}

void ecat_data_publish_inputs(ecat_master_instance_t *inst, int master_index, int wkc,
                              bool operational, bool wkc_ok)
{
    ecat_data_session_t *d = &inst->data;
    const uint8_t *region = ecat_layout_input_region(inst);
    uint32_t in_bytes = inst->layout.input_bytes;

    if (pthread_mutex_trylock(&d->lock) != 0)
        return;
    if (!d->open || d->fd < 0 || d->tx_buf == NULL || d->tx_buf_size < EDOG_FRAME_HEADER_SIZE + in_bytes) {
        pthread_mutex_unlock(&d->lock);
        return;
    }

    uint8_t *buf = d->tx_buf;
    edog_frame_header_t h = {
        .version = EDOG_PROTOCOL_VERSION,
        .kind = EDOG_FRAME_INPUTS,
        .master = (uint8_t)master_index,
        .flags = (uint8_t)((operational ? EDOG_FLAG_VALID : 0) | (wkc_ok ? EDOG_FLAG_WKC_OK : 0)),
        .session = d->session,
        .sequence = ++d->tx_seq,
        .payload_len = (uint16_t)in_bytes,
        .wkc = (uint16_t)(wkc < 0 ? 0 : wkc),
    };
    edog_frame_encode_header(buf, &h);
    if (region != NULL && in_bytes > 0)
        memcpy(buf + EDOG_FRAME_HEADER_SIZE, region, in_bytes);

    ssize_t n = edog_dgram_send(d->fd, buf, EDOG_FRAME_HEADER_SIZE + in_bytes, &d->peer);
    if (n < 0)
        atomic_fetch_add_explicit(&d->frames_dropped, 1, memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&d->frames_tx, 1, memory_order_relaxed);

    pthread_mutex_unlock(&d->lock);
}
