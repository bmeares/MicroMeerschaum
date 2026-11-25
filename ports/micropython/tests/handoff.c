// SPDX-License-Identifier: Apache-2.0
/* One firmware lifetime in the Unix/MicroPython interoperation test. */
#include <stdio.h>
#include <string.h>
#include "micromeerschaum/journal_pipe.h"
#include "micromeerschaum/peer.h"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "failed at %d: %s\n", __LINE__, #expr); return 1; } } while (0)

static int sample(mm_pipe *p, int64_t tick)
{
    const mm_field field = {{"tick", 4}, {.type = MM_VALUE_INT, .as.integer = tick}};
    const mm_row row = {&field, 1};
    mm_sync_result result = mm_pipe_sync_rows(p, &row, 1, (uint64_t)tick * 1000, 1);
    return result.status == MM_JOURNAL_OK && result.confirmed == 1;
}

static int deliver(mm_journal *src, mm_journal *dst)
{
    uint8_t frame[512], receipt[MM_RECEIPT_MAX];
    size_t n, r;
    return mm_peer_batch(src, mm_journal_confirmed(src), 8, frame, sizeof frame, &n) == MM_JOURNAL_OK
        && mm_peer_receive(dst, 1, frame, n, receipt, sizeof receipt, &r) == MM_JOURNAL_OK
        && mm_peer_confirm(src, 2, receipt, r) == MM_JOURNAL_OK;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    bool start = strcmp(argv[1], "start") == 0;
    CHECK(start || strcmp(argv[1], "finish") == 0);
    static const char key[] = "sensor\x1f" "handoff\x1f";
    mm_journal_config cfg = {{1, 7, key, sizeof key - 1}, MM_JOURNAL_ORIGIN, MM_DESTINATION, 2, 2, 0};
    mm_volume_options options = {16, 4096, MM_KEEP_OLD};
    mm_journal *src, *dst;
    CHECK(mm_journal_open_volume(&src, "source.mm", &cfg, &options, start) == MM_JOURNAL_OK);
    cfg.mode = MM_JOURNAL_RECEIVER;
    CHECK(mm_journal_open_volume(&dst, "receiver.mm", &cfg, &options, start) == MM_JOURNAL_OK);
    mm_pipe pipe;
    uint8_t scratch[256], frame[256], saved[256];
    size_t n, m;
    CHECK(mm_pipe_open(&pipe, "sensor", "handoff", NULL, mm_pipe_journal_target(src),
                       scratch, sizeof scratch) == MM_JOURNAL_OK);
    if (start) {
        CHECK(sample(&pipe, 1) && deliver(src, dst) && sample(&pipe, 2));
        CHECK(mm_journal_read(src, 2, frame, sizeof frame, &n) == MM_JOURNAL_OK);
        FILE *f = fopen("second.bin", "wb");
        CHECK(f && fwrite(frame, 1, n, f) == n && fclose(f) == 0);
    } else {
        CHECK(mm_journal_confirmed(src) == 1 && mm_journal_high(src) == 2);
        FILE *f = fopen("second.bin", "rb");
        CHECK(f);
        m = fread(saved, 1, sizeof saved, f);
        CHECK(fclose(f) == 0);
        CHECK(mm_journal_read(src, 2, frame, sizeof frame, &n) == MM_JOURNAL_OK);
        CHECK(n == m && !memcmp(frame, saved, n));
        CHECK(sample(&pipe, 3) && deliver(src, dst) && mm_journal_confirmed(src) == 3);
        for (uint64_t seq = 1; seq <= 3; ++seq) {
            CHECK(mm_journal_read(src, seq, saved, sizeof saved, &m) == MM_JOURNAL_OK);
            CHECK(mm_journal_read(dst, seq, frame, sizeof frame, &n) == MM_JOURNAL_OK);
            CHECK(n == m && !memcmp(frame, saved, n));
            mm_record rec;
            mm_field field;
            size_t used, count;
            CHECK(mm_record_decode(frame, n, &rec, &used) == MM_OK && rec.seq == seq);
            CHECK(mm_document_decode(rec.payload, rec.len, &field, 1, &count) == MM_OK && count == 1);
            CHECK(field.value.type == MM_VALUE_INT && field.value.as.integer == (int64_t)seq);
        }
    }
    mm_journal_close(src);
    mm_journal_close(dst);
    return 0;
}
