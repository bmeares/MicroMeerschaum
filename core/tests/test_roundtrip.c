// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <string.h>

#include "micromeerschaum/mm.h"

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                  \
        }                                                              \
    } while (0)

static const char KEY[] = "plugin:walter\x1f" "radar\x1f" "bench";
static uint8_t buf[1 << 16];

/* Appends one record (header, then payload) as a journal writer would. */
static size_t append(size_t at, mm_kind kind, uint64_t seq, const void *payload, uint32_t len)
{
    if (mm_record_header(buf + at, kind, seq, seq * 1000u, 2, payload, len) != MM_OK) return 0;
    memcpy(buf + at + MM_RECORD_HEADER_SIZE, payload, len);
    return at + MM_RECORD_HEADER_SIZE + len;
}

static int segment_roundtrip(void)
{
    static uint8_t payload[300];
    for (size_t i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i * 7);
    size_t at = mm_segment_header(buf, sizeof buf, KEY, sizeof KEY - 1, 100, 2);
    CHECK(at == 28 + sizeof KEY - 1);
    size_t ends[20];
    for (uint64_t i = 0; i < 20; i++) {
        mm_kind kind = i % 5 == 4 ? MM_KIND_LOSS : MM_KIND_DATA;
        at = append(at, kind, 100 + i, payload, (uint32_t)(i * 15));
        CHECK(at != 0);
        ends[i] = at;
    }

    mm_scan s;
    CHECK(mm_segment_scan(buf, at, &s) == MM_OK);
    CHECK(s.stop == MM_END && s.records == 20 && s.first_seq == 100 && s.last_seq == 119);
    CHECK(s.frontier == at && s.boot == 2 && s.key_len == sizeof KEY - 1);
    CHECK(memcmp(s.key, KEY, s.key_len) == 0);

    memset(buf + at, 0xFF, 64);
    CHECK(mm_segment_scan(buf, at + 64, &s) == MM_OK);
    CHECK(s.stop == MM_BAD_MAGIC && s.records == 20 && s.frontier == at);

    CHECK(mm_segment_scan(buf, at - 1, &s) == MM_OK);
    CHECK(s.stop == MM_TRUNCATED && s.records == 19 && s.last_seq == 118 && s.frontier == ends[18]);

    mm_record rec;
    size_t used;
    CHECK(mm_record_decode(buf + ends[3], at - ends[3], &rec, &used) == MM_OK);
    CHECK(rec.kind == MM_KIND_LOSS && rec.seq == 104 && rec.mono_us == 104000 && rec.len == 60);
    CHECK(used == ends[4] - ends[3] && memcmp(rec.payload, payload, 60) == 0);
    return 0;
}

static int batch_roundtrip(void)
{
    mm_batch b = {.origin = 0x3030F97A2BC4u, .key = KEY, .key_len = sizeof KEY - 1, .hop_count = 2, .count = 3};
    b.hops[0] = (mm_hop){.node = 0x3030F97A2BC4u, .received_us = -5, .transport = MM_TRANSPORT_LOCAL};
    b.hops[1] = (mm_hop){.node = 7, .received_us = 1793000000000000, .transport = MM_TRANSPORT_LTE};
    size_t at = mm_batch_header(buf, sizeof buf, &b);
    CHECK(at == 28 + sizeof KEY - 1 + 48);
    for (uint64_t seq = 41; seq < 44; seq++) {
        at = append(at, MM_KIND_DATA, seq, "frame", 5);
        CHECK(at != 0);
    }

    mm_batch got;
    uint64_t first = 0, last = 0;
    CHECK(mm_batch_validate(buf, at, &got, &first, &last) == MM_OK);
    CHECK(first == 41 && last == 43 && got.count == 3 && got.origin == b.origin);
    CHECK(got.hop_count == 2 && got.hops[0].received_us == -5 && got.hops[1].transport == MM_TRANSPORT_LTE);
    CHECK(got.key_len == b.key_len && memcmp(got.key, KEY, got.key_len) == 0);
    CHECK(mm_batch_validate(buf, at - 1, &got, &first, &last) == MM_TRUNCATED);
    return 0;
}

static int encoders_reject_bad_input(void)
{
    uint8_t header[MM_RECORD_HEADER_SIZE];
    CHECK(mm_record_header(header, MM_KIND_DATA, 0, 0, 0, "", 0) == MM_BAD_SEQ);
    CHECK(mm_record_header(header, (mm_kind)2, 1, 0, 0, "", 0) == MM_BAD_KIND);
    CHECK(mm_record_header(header, MM_KIND_DATA, 1, 0, 0, buf, MM_RECORD_MAX_PAYLOAD + 1) == MM_BAD_LENGTH);
    CHECK(mm_segment_header(buf, sizeof buf, KEY, sizeof KEY - 1, 0, 0) == 0);
    CHECK(mm_segment_header(buf, 28 + sizeof KEY - 2, KEY, sizeof KEY - 1, 1, 0) == 0);
    CHECK(mm_segment_header(buf, sizeof buf, "radar", 5, 1, 0) == 0);
    mm_batch b = {.key = KEY, .key_len = sizeof KEY - 1, .hop_count = MM_BATCH_MAX_HOPS + 1};
    CHECK(mm_batch_header(buf, sizeof buf, &b) == 0);
    b.hop_count = 1;
    b.hops[0].transport = (mm_transport)5;
    CHECK(mm_batch_header(buf, sizeof buf, &b) == 0);
    return 0;
}

int main(void)
{
    int failed = segment_roundtrip() | batch_roundtrip() | encoders_reject_bad_input();
    printf("roundtrip: %s\n", failed ? "FAILED" : "ok");
    return failed;
}
