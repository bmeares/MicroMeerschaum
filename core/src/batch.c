// SPDX-License-Identifier: Apache-2.0
#include <string.h>

#include "le.h"
#include "micromeerschaum/mm.h"

#define HOP_SIZE 24u

size_t mm_batch_header(uint8_t *out, size_t cap, const mm_batch *batch)
{
    if (mm_key_check(batch->key, batch->key_len) != MM_OK) return 0;
    if (batch->hop_count > MM_BATCH_MAX_HOPS) return 0;
    for (size_t i = 0; i < batch->hop_count; i++) {
        if (batch->hops[i].transport > MM_TRANSPORT_USB) return 0;
    }
    size_t size = 28 + batch->key_len + HOP_SIZE * batch->hop_count;
    if (cap < size) return 0;
    memcpy(out, "MMBT", 4);
    out[4] = MM_WIRE_VERSION;
    out[5] = (uint8_t)batch->hop_count;
    out[6] = out[7] = 0;
    mm_put64(out + 8, batch->origin);
    mm_put32(out + 16, (uint32_t)batch->key_len);
    memcpy(out + 20, batch->key, batch->key_len);
    uint8_t *p = out + 20 + batch->key_len;
    for (size_t i = 0; i < batch->hop_count; i++, p += HOP_SIZE) {
        mm_put64(p, batch->hops[i].node);
        mm_put64(p + 8, (uint64_t)batch->hops[i].received_us);
        p[16] = (uint8_t)batch->hops[i].transport;
        memset(p + 17, 0, 7);
    }
    mm_put32(p, batch->count);
    mm_put32(p + 4, mm_crc32(0, out, size - 4));
    return size;
}

mm_status mm_batch_header_decode(const uint8_t *buf, size_t len, mm_batch *batch,
                                 size_t *header_size)
{
    if (len < 20) return MM_TRUNCATED;
    if (memcmp(buf, "MMBT", 4) != 0) return MM_BAD_MAGIC;
    if (buf[4] != MM_WIRE_VERSION) return MM_BAD_VERSION;
    size_t hop_count = buf[5];
    if (hop_count > MM_BATCH_MAX_HOPS) return MM_BAD_FIELD;
    if (!mm_all_zero(buf + 6, 2)) return MM_BAD_FIELD;
    uint32_t key_len = mm_get32(buf + 16);
    if (key_len < 1 || key_len > MM_KEY_MAX) return MM_BAD_KEY;
    size_t size = 28 + (size_t)key_len + HOP_SIZE * hop_count;
    if (len < size) return MM_TRUNCATED;
    if (mm_crc32(0, buf, size - 4) != mm_get32(buf + size - 4)) return MM_BAD_CRC;
    mm_batch b;
    b.key = (const char *)(buf + 20);
    b.key_len = key_len;
    if (mm_key_check(b.key, b.key_len) != MM_OK) return MM_BAD_KEY;
    const uint8_t *p = buf + 20 + b.key_len;
    for (size_t i = 0; i < hop_count; i++, p += HOP_SIZE) {
        if (p[16] > MM_TRANSPORT_USB) return MM_BAD_FIELD;
        if (!mm_all_zero(p + 17, 7)) return MM_BAD_FIELD;
        b.hops[i].node = mm_get64(p);
        b.hops[i].received_us = mm_get_i64(p + 8);
        b.hops[i].transport = (mm_transport)p[16];
    }
    b.origin = mm_get64(buf + 8);
    b.hop_count = hop_count;
    b.count = mm_get32(p);
    *batch = b;
    *header_size = size;
    return MM_OK;
}

mm_status mm_batch_validate(const uint8_t *buf, size_t len, mm_batch *batch,
                            uint64_t *first_seq, uint64_t *last_seq)
{
    mm_batch b;
    size_t off;
    mm_status st = mm_batch_header_decode(buf, len, &b, &off);
    if (st != MM_OK) return st;
    uint64_t first = 0, last = 0;
    for (uint32_t i = 0; i < b.count; i++) {
        mm_record rec;
        size_t used;
        st = mm_record_decode(buf + off, len - off, &rec, &used);
        if (st != MM_OK) return st;
        if (i == 0) {
            first = rec.seq;
        } else if (rec.seq != last + 1) {
            return MM_SEQ_GAP;
        }
        last = rec.seq;
        off += used;
    }
    if (off != len) return MM_TRAILING;
    *batch = b;
    *first_seq = first;
    *last_seq = last;
    return MM_OK;
}
