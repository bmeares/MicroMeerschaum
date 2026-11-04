// SPDX-License-Identifier: Apache-2.0
#include "le.h"
#include "micromeerschaum/mm.h"

mm_status mm_record_header(uint8_t out[MM_RECORD_HEADER_SIZE], mm_kind kind, uint64_t seq,
                           uint64_t mono_us, uint32_t boot, const void *payload, uint32_t len)
{
    if (kind != MM_KIND_DATA && kind != MM_KIND_LOSS) return MM_BAD_KIND;
    if (len > MM_RECORD_MAX_PAYLOAD) return MM_BAD_LENGTH;
    if (seq == 0) return MM_BAD_SEQ;
    out[0] = 'M';
    out[1] = 'R';
    out[2] = MM_WIRE_VERSION;
    out[3] = (uint8_t)kind;
    mm_put32(out + 4, len);
    mm_put64(out + 8, seq);
    mm_put64(out + 16, mono_us);
    mm_put32(out + 24, boot);
    mm_put32(out + 28, mm_crc32(mm_crc32(0, out, 28), payload, len));
    return MM_OK;
}

mm_status mm_record_decode(const uint8_t *buf, size_t len, mm_record *rec, size_t *consumed)
{
    if (len < MM_RECORD_HEADER_SIZE) return MM_TRUNCATED;
    if (buf[0] != 'M' || buf[1] != 'R') return MM_BAD_MAGIC;
    if (buf[2] != MM_WIRE_VERSION) return MM_BAD_VERSION;
    if (buf[3] > MM_KIND_LOSS) return MM_BAD_KIND;
    uint32_t n = mm_get32(buf + 4);
    if (n > MM_RECORD_MAX_PAYLOAD) return MM_BAD_LENGTH;
    if (len - MM_RECORD_HEADER_SIZE < n) return MM_TRUNCATED;
    const uint8_t *payload = buf + MM_RECORD_HEADER_SIZE;
    if (mm_crc32(mm_crc32(0, buf, 28), payload, n) != mm_get32(buf + 28)) return MM_BAD_CRC;
    uint64_t seq = mm_get64(buf + 8);
    if (seq == 0) return MM_BAD_SEQ;
    rec->kind = (mm_kind)buf[3];
    rec->seq = seq;
    rec->mono_us = mm_get64(buf + 16);
    rec->boot = mm_get32(buf + 24);
    rec->len = n;
    rec->payload = payload;
    *consumed = MM_RECORD_HEADER_SIZE + (size_t)n;
    return MM_OK;
}
