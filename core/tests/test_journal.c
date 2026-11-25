// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include "micromeerschaum/journal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static const char key[] = "sensor\x1ftemp\x1f";
static const mm_journal_config origin = {{1, 5, key, sizeof(key)-1}, MM_JOURNAL_ORIGIN,
                                         MM_DESTINATION, 2, 2, 0};

static void cleanup(const char *path)
{
    DIR *dir = opendir(path);
    CHECK(dir);
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        CHECK(unlinkat(dirfd(dir), ent->d_name, 0) == 0);
    }
    CHECK(closedir(dir) == 0);
    CHECK(rmdir(path) == 0);
}

static size_t frame(uint8_t *out, uint64_t seq, uint8_t value)
{
    out[MM_RECORD_HEADER_SIZE] = value;
    CHECK(mm_record_header(out, MM_KIND_DATA, seq, seq, 1, out + MM_RECORD_HEADER_SIZE, 1) == MM_OK);
    return MM_RECORD_HEADER_SIZE + 1;
}

static size_t declaration(uint8_t *out, uint64_t seq, mm_loss loss)
{
    CHECK(mm_loss_encode(out + MM_RECORD_HEADER_SIZE, &loss, seq) == MM_OK);
    CHECK(mm_record_header(out, MM_KIND_LOSS, seq, seq, 1, out + MM_RECORD_HEADER_SIZE, MM_LOSS_SIZE) == MM_OK);
    return MM_RECORD_HEADER_SIZE + MM_LOSS_SIZE;
}

static void origin_state(void)
{
    char path[] = "/tmp/mm-origin-XXXXXX";
    CHECK(mkdtemp(path));
    mm_journal *j, *other;
    CHECK(mm_journal_open(&j, path, &origin, false) == MM_JOURNAL_NOT_FOUND);
    CHECK(mm_journal_open(&j, path, &origin, true) == MM_JOURNAL_OK);
    CHECK(mm_journal_open(&other, path, &origin, false) == MM_JOURNAL_BUSY);
    const mm_sample group[] = {{MM_KIND_DATA, 1, 1, "a", 1}, {MM_KIND_DATA, 2, 1, "b", 1}};
    uint64_t seq;
    CHECK(mm_journal_append_group(j, group, 2, &seq) == MM_JOURNAL_OK && seq == 1);
    const mm_sample invalid[] = {group[0], {MM_KIND_LOSS, 2, 1, "xyz", 3}};
    CHECK(mm_journal_append_group(j, invalid, 2, &seq) == MM_JOURNAL_INVALID);
    CHECK(mm_journal_high(j) == 2 && mm_journal_prefix(j) == 2);
    uint8_t buf[40];
    size_t used;
    CHECK(mm_journal_read(j, 2, NULL, 0, &used) == MM_JOURNAL_TOO_SMALL && used == 33);
    CHECK(mm_journal_read(j, 2, buf, sizeof(buf), &used) == MM_JOURNAL_OK && buf[32] == 'b');
    mm_receipt ack = {origin.stream, MM_DESTINATION, 2, 2, 3};
    CHECK(mm_journal_confirm(j, &ack) == MM_JOURNAL_INVALID);
    ack.through = 1;
    CHECK(mm_journal_confirm(j, &ack) == MM_JOURNAL_OK);
    CHECK(mm_journal_reclaim(j) == MM_JOURNAL_OK);
    CHECK(mm_journal_read(j, 1, buf, sizeof(buf), &used) == MM_JOURNAL_OK);
    ack.through = 2;
    CHECK(mm_journal_confirm(j, &ack) == MM_JOURNAL_OK);
    ack.through = 1;
    CHECK(mm_journal_confirm(j, &ack) == MM_JOURNAL_OK && mm_journal_confirmed(j) == 2);
    CHECK(mm_journal_reclaim(j) == MM_JOURNAL_OK);
    CHECK(mm_journal_read(j, 1, buf, sizeof(buf), &used) == MM_JOURNAL_NOT_FOUND);
    mm_journal_close(j);
    mm_journal_config different = origin;
    ++different.stream.generation;
    CHECK(mm_journal_open(&other, path, &different, true) == MM_JOURNAL_CONFLICT);
    different = origin;
    different.issuer = different.destination = 12;
    CHECK(mm_journal_open(&other, path, &different, true) == MM_JOURNAL_CONFLICT);
    CHECK(mm_journal_open(&j, path, &origin, false) == MM_JOURNAL_OK);
    CHECK(mm_journal_prefix(j) == 2 && mm_journal_high(j) == 2 && mm_journal_confirmed(j) == 2);
    CHECK(mm_journal_append(j, MM_KIND_DATA, 3, 1, "c", 1, &seq) == MM_JOURNAL_OK && seq == 3);
    mm_journal_close(j);
    cleanup(path);
}

static void overlap_and_capacity(void)
{
    char path[] = "/tmp/mm-overlap-XXXXXX";
    CHECK(mkdtemp(path));
    mm_journal_config config = origin;
    config.mode = MM_JOURNAL_RECEIVER;
    const size_t h = 28 + config.stream.key_len;
    /* Two singletons, then three disjoint gaps. Physical budget holds exactly
     * five singleton segments. Duplicating an existing frame would exceed it. */
    config.max_bytes = 5 * (h + 33);
    mm_journal *j;
    CHECK(mm_journal_open(&j, path, &config, true) == MM_JOURNAL_OK);
    uint8_t batch[165];
    for (uint8_t i = 0; i < 5; ++i) frame(batch + 33*i, i+1u, i);
    CHECK(mm_journal_ingest(j, batch+33, 33) == MM_JOURNAL_OK);
    CHECK(mm_journal_ingest(j, batch+99, 33) == MM_JOURNAL_OK);
    CHECK(mm_journal_prefix(j) == 0);
    frame(batch+99, 4, 17);
    CHECK(mm_journal_ingest(j, batch, sizeof(batch)) == MM_JOURNAL_CONFLICT);
    size_t used;
    CHECK(mm_journal_read(j, 1, NULL, 0, &used) == MM_JOURNAL_NOT_FOUND);
    frame(batch+99, 4, 3);
    CHECK(mm_journal_ingest(j, batch, sizeof(batch)) == MM_JOURNAL_OK);
    CHECK(mm_journal_ingest(j, batch, sizeof(batch)) == MM_JOURNAL_OK);
    uint8_t extra[33];
    frame(extra, 6, 9);
    CHECK(mm_journal_ingest(j, extra, sizeof(extra)) == MM_JOURNAL_FULL);
    CHECK(mm_journal_reclaim(j) == MM_JOURNAL_INVALID);
    mm_journal_close(j);
    CHECK(mm_journal_open(&j, path, &config, false) == MM_JOURNAL_OK);
    CHECK(mm_journal_prefix(j) == 5 && mm_journal_high(j) == 5);
    for (uint8_t i = 0; i < 5; ++i) {
        CHECK(mm_journal_read(j, i+1u, extra, sizeof(extra), &used) == MM_JOURNAL_OK);
        CHECK(used == 33 && !memcmp(extra, batch + 33*i, 33));
    }
    mm_journal_close(j);
    cleanup(path);
}

static void loss_coverage(void)
{
    char path[] = "/tmp/mm-loss-XXXXXX";
    CHECK(mkdtemp(path));
    mm_journal_config c = origin;
    c.mode = MM_JOURNAL_RECEIVER;
    c.role = MM_CUSTODY;
    c.destination = 19;
    mm_journal *j;
    CHECK(mm_journal_open(&j, path, &c, true) == MM_JOURNAL_OK);
    uint8_t buf[MM_RECORD_HEADER_SIZE + MM_LOSS_SIZE];
    size_t n = declaration(buf, 4, (mm_loss){MM_LOSS_MISSED, 42, 0});
    CHECK(mm_journal_ingest(j, buf, n) == MM_JOURNAL_OK && mm_journal_prefix(j) == 0);
    n = frame(buf, 1, 1);
    CHECK(mm_journal_ingest(j, buf, n) == MM_JOURNAL_OK && mm_journal_prefix(j) == 1);
    n = declaration(buf, 5, (mm_loss){MM_LOSS_EVICTED, 2, 3});
    CHECK(mm_journal_ingest(j, buf, n) == MM_JOURNAL_OK && mm_journal_prefix(j) == 5);
    mm_journal_close(j);
    CHECK(mm_journal_open(&j, path, &c, false) == MM_JOURNAL_OK && mm_journal_prefix(j) == 5);
    CHECK(mm_journal_read(j, 2, buf, sizeof(buf), &n) == MM_JOURNAL_NOT_FOUND);
    n = frame(buf, 2, 2);
    CHECK(mm_journal_ingest(j, buf, n) == MM_JOURNAL_OK);
    CHECK(mm_journal_read(j, 2, buf, sizeof(buf), &n) == MM_JOURNAL_OK && buf[32] == 2);
    c.destination = 20;
    mm_journal_close(j);
    CHECK(mm_journal_open(&j, path, &c, false) == MM_JOURNAL_CONFLICT);
    cleanup(path);
}

typedef struct { unsigned calls, fail_at; } fault;
static int fail_sync(int fd, void *context)
{
    fault *f = context;
    if (++f->calls == f->fail_at) { errno = EIO; return -1; }
    return fsync(fd);
}

static void barrier_windows(void)
{
    char path[] = "/tmp/mm-barrier-XXXXXX";
    CHECK(mkdtemp(path));
    mm_journal *j;
    CHECK(mm_journal_open(&j, path, &origin, true) == MM_JOURNAL_OK);
    uint64_t seq;
    fault f = {0, 1};
    mm_journal_set_barrier(j, fail_sync, &f);
    CHECK(mm_journal_append(j, MM_KIND_DATA, 1, 1, "a", 1, &seq) == MM_JOURNAL_UNCERTAIN && seq == 1);
    CHECK(mm_journal_append(j, MM_KIND_DATA, 1, 1, "a", 1, &seq) == MM_JOURNAL_UNCERTAIN && seq == 0);
    mm_journal_close(j);
    CHECK(mm_journal_open(&j, path, &origin, false) == MM_JOURNAL_OK && mm_journal_high(j) == 0);
    f = (fault){0, 2};
    mm_journal_set_barrier(j, fail_sync, &f);
    CHECK(mm_journal_append(j, MM_KIND_DATA, 1, 1, "a", 1, &seq) == MM_JOURNAL_UNCERTAIN && seq == 1);
    mm_journal_close(j);
    CHECK(mm_journal_open(&j, path, &origin, false) == MM_JOURNAL_OK && mm_journal_high(j) == 1);
    uint8_t bytes[40];
    size_t n;
    CHECK(mm_journal_read(j, 1, bytes, sizeof(bytes), &n) == MM_JOURNAL_OK && bytes[32] == 'a');
    f = (fault){0, 1};
    mm_journal_set_barrier(j, fail_sync, &f);
    mm_receipt r = {origin.stream, MM_DESTINATION, 2, 2, 1};
    CHECK(mm_journal_confirm(j, &r) == MM_JOURNAL_UNCERTAIN);
    CHECK(mm_journal_needs_reopen(j));
    CHECK(mm_journal_reclaim(j) == MM_JOURNAL_UNCERTAIN);
    mm_journal_close(j);
    CHECK(mm_journal_open(&j, path, &origin, false) == MM_JOURNAL_OK && mm_journal_confirmed(j) == 0);
    CHECK(!mm_journal_needs_reopen(j));
    CHECK(mm_journal_read(j, 1, bytes, sizeof(bytes), &n) == MM_JOURNAL_OK);
    f = (fault){0, 2};
    mm_journal_set_barrier(j, fail_sync, &f);
    CHECK(mm_journal_confirm(j, &r) == MM_JOURNAL_UNCERTAIN);
    mm_journal_close(j);
    CHECK(mm_journal_open(&j, path, &origin, false) == MM_JOURNAL_OK && mm_journal_confirmed(j) == 1);
    f = (fault){0, 1};
    mm_journal_set_barrier(j, fail_sync, &f);
    CHECK(mm_journal_reclaim(j) == MM_JOURNAL_UNCERTAIN);
    mm_journal_close(j);
    CHECK(mm_journal_open(&j, path, &origin, false) == MM_JOURNAL_OK && mm_journal_confirmed(j) == 1);
    CHECK(mm_journal_read(j, 1, bytes, sizeof(bytes), &n) == MM_JOURNAL_NOT_FOUND);
    CHECK(mm_journal_append(j, MM_KIND_DATA, 2, 1, "b", 1, &seq) == MM_JOURNAL_OK && seq == 2);
    mm_journal_close(j);
    cleanup(path);
}

static void damaged_store(void)
{
    char path[] = "/tmp/mm-damage-XXXXXX";
    CHECK(mkdtemp(path));
    mm_journal *j;
    CHECK(mm_journal_open(&j, path, &origin, true) == MM_JOURNAL_OK);
    uint64_t seq;
    CHECK(mm_journal_append(j, MM_KIND_DATA, 1, 1, "a", 1, &seq) == MM_JOURNAL_OK);
    mm_journal_close(j);
    int dir = open(path, O_RDONLY | O_DIRECTORY);
    CHECK(dir >= 0);
    int tmp = openat(dir, ".journal.pending", O_CREAT | O_WRONLY, 0600);
    CHECK(tmp >= 0 && write(tmp, "torn", 4) == 4 && close(tmp) == 0);
    CHECK(mm_journal_open(&j, path, &origin, false) == MM_JOURNAL_OK && mm_journal_high(j) == 1);
    mm_journal_close(j);
    int data = openat(dir, "0000000000000001-0000000000000001.seg", O_RDWR);
    CHECK(data >= 0 && lseek(data, -1, SEEK_END) >= 0 && write(data, "X", 1) == 1 && close(data) == 0);
    CHECK(mm_journal_open(&j, path, &origin, true) == MM_JOURNAL_CORRUPT);
    CHECK(unlinkat(dir, "journal.meta", 0) == 0);
    CHECK(mm_journal_open(&j, path, &origin, true) == MM_JOURNAL_CORRUPT);
    close(dir);
    cleanup(path);
}

int main(void)
{
    origin_state();
    overlap_and_capacity();
    loss_coverage();
    barrier_windows();
    damaged_store();
    return 0;
}
