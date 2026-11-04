// SPDX-License-Identifier: Apache-2.0
#include <string.h>

#include "le.h"
#include "micromeerschaum/mm.h"

size_t mm_segment_header(uint8_t *out, size_t cap, const char *key, size_t key_len,
                         uint64_t first_seq, uint32_t boot)
{
    if (mm_key_check(key, key_len) != MM_OK || first_seq == 0) return 0;
    size_t size = 28 + key_len;
    if (cap < size) return 0;
    memcpy(out, "MMSG", 4);
    out[4] = MM_WIRE_VERSION;
    out[5] = out[6] = out[7] = 0;
    mm_put64(out + 8, first_seq);
    mm_put32(out + 16, boot);
    mm_put32(out + 20, (uint32_t)key_len);
    memcpy(out + 24, key, key_len);
    mm_put32(out + size - 4, mm_crc32(0, out, size - 4));
    return size;
}

mm_status mm_segment_scan(const uint8_t *data, size_t len, mm_scan *out)
{
    if (len < 24) return MM_TRUNCATED;
    if (memcmp(data, "MMSG", 4) != 0) return MM_BAD_MAGIC;
    if (data[4] != MM_WIRE_VERSION) return MM_BAD_VERSION;
    if (!mm_all_zero(data + 5, 3)) return MM_BAD_FIELD;
    uint32_t key_len = mm_get32(data + 20);
    if (key_len < 1 || key_len > MM_KEY_MAX) return MM_BAD_KEY;
    size_t size = 28 + (size_t)key_len;
    if (len < size) return MM_TRUNCATED;
    if (mm_crc32(0, data, size - 4) != mm_get32(data + size - 4)) return MM_BAD_CRC;
    mm_scan s;
    s.first_seq = mm_get64(data + 8);
    if (s.first_seq == 0) return MM_BAD_SEQ;
    s.key = (const char *)(data + 24);
    s.key_len = key_len;
    if (mm_key_check(s.key, s.key_len) != MM_OK) return MM_BAD_KEY;
    s.header_size = size;
    s.boot = mm_get32(data + 16);
    s.records = 0;
    s.frontier = size;
    s.stop = MM_END;
    while (s.frontier < len) {
        mm_record rec;
        size_t used;
        mm_status st = mm_record_decode(data + s.frontier, len - s.frontier, &rec, &used);
        if (st == MM_OK && rec.seq != s.first_seq + s.records) st = MM_SEQ_GAP;
        if (st != MM_OK) {
            s.stop = st;
            break;
        }
        s.frontier += used;
        s.records++;
    }
    s.last_seq = s.first_seq + s.records - 1;
    *out = s;
    return MM_OK;
}
