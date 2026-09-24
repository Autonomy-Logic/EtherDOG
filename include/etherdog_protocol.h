// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Autonomy®

/**
 * @file etherdog_protocol.h
 * @brief EtherDOG cyclic data frame format (docs/PROTOCOL.md).
 *
 * Every frame is one datagram: a fixed 24-byte little-endian header followed by the raw
 * bytes of one direction's process image region. Fields are read and written through the
 * helpers below, never through a struct overlay, so no side depends on compiler packing.
 *
 *   offset size field
 *        0    4 magic        "EDOG" (0x45 0x44 0x4F 0x47)
 *        4    1 version      EDOG_PROTOCOL_VERSION
 *        5    1 kind         EDOG_FRAME_OUTPUTS (client -> EtherDOG) or EDOG_FRAME_INPUTS
 *        6    1 master       master index from the layout
 *        7    1 flags        EDOG_FLAG_*
 *        8    8 session      value returned by "open_data"
 *       16    4 sequence     increments by one per frame sent, wraps
 *       20    2 payload_len  bytes following the header
 *       22    2 wkc          INPUTS: last working counter; OUTPUTS: 0
 */

#ifndef ETHERDOG_PROTOCOL_H
#define ETHERDOG_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDOG_PROTOCOL_VERSION 1
#define EDOG_FRAME_HEADER_SIZE 24
#define EDOG_FRAME_MAX_PAYLOAD 4096

enum {
    EDOG_FRAME_OUTPUTS = 1,
    EDOG_FRAME_INPUTS = 2,
};

/* INPUTS: the bus is OPERATIONAL. OUTPUTS: the payload holds valid outputs (clear it to
 * ask EtherDOG to drive the safe state, all outputs zero). */
#define EDOG_FLAG_VALID 0x01
/* INPUTS only: the working counter matched the expected value for this cycle. */
#define EDOG_FLAG_WKC_OK 0x02

typedef struct {
    uint8_t version;
    uint8_t kind;
    uint8_t master;
    uint8_t flags;
    uint64_t session;
    uint32_t sequence;
    uint16_t payload_len;
    uint16_t wkc;
} edog_frame_header_t;

static inline void edog_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void edog_put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static inline void edog_put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static inline uint16_t edog_get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t edog_get_u32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static inline uint64_t edog_get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

/** Write @p h into the first EDOG_FRAME_HEADER_SIZE bytes of @p out. */
static inline void edog_frame_encode_header(uint8_t *out, const edog_frame_header_t *h)
{
    out[0] = 'E';
    out[1] = 'D';
    out[2] = 'O';
    out[3] = 'G';
    out[4] = h->version;
    out[5] = h->kind;
    out[6] = h->master;
    out[7] = h->flags;
    edog_put_u64(out + 8, h->session);
    edog_put_u32(out + 16, h->sequence);
    edog_put_u16(out + 20, h->payload_len);
    edog_put_u16(out + 22, h->wkc);
}

/**
 * Parse a received datagram of @p len bytes. Checks the magic, the version and that the
 * declared payload length matches what arrived. Does not check the session or the kind.
 */
static inline bool edog_frame_decode_header(const uint8_t *in, size_t len,
                                            edog_frame_header_t *h)
{
    if (len < EDOG_FRAME_HEADER_SIZE)
        return false;
    if (in[0] != 'E' || in[1] != 'D' || in[2] != 'O' || in[3] != 'G')
        return false;
    h->version = in[4];
    h->kind = in[5];
    h->master = in[6];
    h->flags = in[7];
    h->session = edog_get_u64(in + 8);
    h->sequence = edog_get_u32(in + 16);
    h->payload_len = edog_get_u16(in + 20);
    h->wkc = edog_get_u16(in + 22);
    if (h->version != EDOG_PROTOCOL_VERSION)
        return false;
    if (h->payload_len > EDOG_FRAME_MAX_PAYLOAD)
        return false;
    return len == (size_t)EDOG_FRAME_HEADER_SIZE + h->payload_len;
}

/** True when @p seq is newer than @p last in wrap-around order. */
static inline bool edog_seq_newer(uint32_t seq, uint32_t last)
{
    return (int32_t)(seq - last) > 0;
}

#endif /* ETHERDOG_PROTOCOL_H */
