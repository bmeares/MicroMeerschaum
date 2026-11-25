// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include "micromeerschaum/peer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static const char key[] = "sensor\x1ftemp\x1f";
static const mm_journal_config origin = {{1, 5, key, sizeof(key)-1}, MM_JOURNAL_ORIGIN,
                                         MM_DESTINATION, 2, 2, 0};

static void group_survives_remount(const char *path)
{
    mm_volume_options opt = {4, 512, MM_KEEP_OLD};
    mm_journal *j = NULL;
    CHECK(mm_journal_open_volume(&j, path, &origin, &opt, true) == MM_JOURNAL_OK);
    mm_sample group[] = {{MM_KIND_DATA, 1, 1, "a", 1}, {MM_KIND_DATA, 2, 1, "b", 1}};
    uint64_t first;
    CHECK(mm_journal_append_group(j, group, 2, &first) == MM_JOURNAL_OK && first == 1);
    mm_journal_close(j);
    CHECK(mm_journal_open_volume(&j, path, &origin, &opt, false) == MM_JOURNAL_OK);
    uint8_t buf[33];
    size_t n;
    CHECK(mm_journal_read(j, 2, buf, sizeof(buf), &n) == MM_JOURNAL_OK && buf[32] == 'b');
    CHECK(mm_journal_high(j) == 2 && mm_journal_prefix(j) == 2);
    mm_journal_close(j);
}

static mm_loss read_loss(mm_journal *j, uint64_t seq)
{
    uint8_t buf[MM_RECORD_HEADER_SIZE + MM_LOSS_SIZE];
    size_t n;
    mm_record rec;
    mm_loss loss;
    CHECK(mm_journal_read(j, seq, buf, sizeof(buf), &n) == MM_JOURNAL_OK);
    CHECK(mm_record_decode(buf, n, &rec, &n) == MM_OK && rec.kind == MM_KIND_LOSS);
    CHECK(mm_loss_decode(rec.payload, rec.len, seq, &loss) == MM_OK);
    return loss;
}

static void keep_old(const char *path)
{
    const mm_volume_options opt = {2, 512, MM_KEEP_OLD};
    mm_journal *j;
    uint64_t first;
    CHECK(mm_journal_open_volume(&j, path, &origin, &opt, true) == MM_JOURNAL_OK);
    CHECK(mm_journal_append(j, MM_KIND_DATA, 1, 1, "a", 1, &first) == MM_JOURNAL_OK);
    CHECK(mm_journal_append(j, MM_KIND_DATA, 2, 1, "b", 1, &first) == MM_JOURNAL_OK);
    mm_sample group[] = {{MM_KIND_DATA, 3, 1, "c", 1}, {MM_KIND_DATA, 4, 1, "d", 1}};
    CHECK(mm_journal_append_group(j, group, 2, &first) == MM_JOURNAL_FULL && first == 0);
    mm_journal_close(j);
    CHECK(mm_journal_open_volume(&j, path, &origin, &opt, false) == MM_JOURNAL_OK);
    CHECK(mm_journal_high(j) == 2 && mm_journal_missed(j) == 2);
    mm_receipt ack = {origin.stream, MM_DESTINATION, 2, 2, 1};
    CHECK(mm_journal_confirm(j, &ack) == MM_JOURNAL_OK);
    CHECK(mm_journal_reclaim(j) == MM_JOURNAL_OK);
    mm_journal_close(j);
    CHECK(mm_journal_open_volume(&j, path, &origin, &opt, false) == MM_JOURNAL_OK);
    CHECK(mm_journal_missed(j) == 0 && mm_journal_high(j) == 3);
    mm_loss loss = read_loss(j, 3);
    CHECK(loss.kind == MM_LOSS_MISSED && loss.first == 2 && loss.last == 0);
    mm_journal_close(j);
}

static void keep_new(const char *path)
{
    const mm_volume_options opt = {2, 512, MM_KEEP_NEW};
    mm_journal *j;
    uint64_t first;
    CHECK(mm_journal_open_volume(&j, path, &origin, &opt, true) == MM_JOURNAL_OK);
    CHECK(mm_journal_append(j, MM_KIND_DATA, 1, 1, "a", 1, &first) == MM_JOURNAL_OK);
    CHECK(mm_journal_append(j, MM_KIND_DATA, 2, 1, "b", 1, &first) == MM_JOURNAL_OK);
    CHECK(mm_journal_append(j, MM_KIND_DATA, 3, 1, "c", 1, &first) == MM_JOURNAL_OK && first == 3);
    mm_journal_close(j);
    CHECK(mm_journal_open_volume(&j, path, &origin, &opt, false) == MM_JOURNAL_OK);
    CHECK(mm_journal_high(j) == 4 && mm_journal_prefix(j) == 4);
    mm_loss loss = read_loss(j, 4);
    CHECK(loss.kind == MM_LOSS_EVICTED && loss.first == 1 && loss.last == 1);
    size_t n;
    CHECK(mm_journal_read(j, 1, NULL, 0, &n) == MM_JOURNAL_NOT_FOUND);
    CHECK(mm_journal_next(j, 1, &first) == MM_JOURNAL_OK && first == 2);
    CHECK(mm_journal_append(j, MM_KIND_DATA, 5, 1, "e", 1, &first) == MM_JOURNAL_OK && first == 5);
    CHECK(mm_journal_append(j, MM_KIND_DATA, 7, 1, "g", 1, &first) == MM_JOURNAL_OK && first == 7);
    loss = read_loss(j, 8);
    CHECK(loss.kind == MM_LOSS_EVICTED && loss.first == 1 && loss.last == 4);
    char peer_path[160];
    snprintf(peer_path, sizeof(peer_path), "%s.peer", path);
    mm_journal_config c = origin;
    c.mode = MM_JOURNAL_RECEIVER;
    mm_journal *peer;
    const mm_volume_options receiver = {8, 512, MM_KEEP_NEW};
    CHECK(mm_journal_open_volume(&peer, peer_path, &c, &receiver, true) == MM_JOURNAL_OK);
    for (int attempt = 0; attempt < 8 && mm_journal_confirmed(j) < 8; ++attempt) {
        uint8_t batch[256], reply[256];
        size_t used, reply_size;
        CHECK(mm_peer_batch(j, mm_journal_confirmed(j), 1, batch, sizeof(batch), &used) == MM_JOURNAL_OK);
        CHECK(mm_peer_receive(peer, 1, batch, used, reply, sizeof(reply), &reply_size) == MM_JOURNAL_OK);
        CHECK(mm_peer_confirm(j, 2, reply, reply_size) == MM_JOURNAL_OK);
    }
    CHECK(mm_journal_confirmed(j) == 8 && mm_journal_prefix(peer) == 8);
    mm_journal_close(peer);
    CHECK(unlink(peer_path) == 0);
    mm_journal_close(j);
}

static void damaged_checkpoint(const char *path)
{
    group_survives_remount(path);
    const mm_volume_options opt = {4, 512, MM_KEEP_OLD};
    /* Damage opposite commit pages in succession. Opening and validating
     * the survivor repairs its partner before another successful append. */
    for (off_t at = 0; at <= 4096; at += 4096) {
        int fd = open(path, O_RDWR);
        CHECK(fd >= 0 && pwrite(fd, "X", 1, at) == 1 && fsync(fd) == 0 && close(fd) == 0);
        mm_journal *j;
        CHECK(mm_journal_open_volume(&j, path, &origin, &opt, false) == MM_JOURNAL_OK);
        CHECK(mm_journal_high(j) == 2);
        mm_journal_close(j);
    }
}

typedef struct { unsigned left; } fault;
static int fail_sync(int fd, void *context)
{
    fault *f = context;
    if (--f->left == 0) { errno = EIO; return -1; }
    return fsync(fd);
}

static void interrupted_eviction(const char *path)
{
    const mm_volume_options opt = {2, 512, MM_KEEP_NEW};
    for (unsigned barrier = 1; barrier <= 3; ++barrier) {
        uint64_t seq;
        mm_journal *j;
        CHECK(mm_journal_open_volume(&j, path, &origin, &opt, true) == MM_JOURNAL_OK);
        CHECK(mm_journal_append(j, MM_KIND_DATA, 0, 1, "a", 1, &seq) == MM_JOURNAL_OK);
        CHECK(mm_journal_append(j, MM_KIND_DATA, 0, 1, "b", 1, &seq) == MM_JOURNAL_OK);
        fault f = {barrier};
        mm_journal_set_barrier(j, fail_sync, &f);
        CHECK(mm_journal_append(j, MM_KIND_DATA, 0, 1, "c", 1, &seq) == MM_JOURNAL_UNCERTAIN && seq == 3);
        CHECK(mm_journal_needs_reopen(j));
        size_t n;
        CHECK(mm_journal_read(j, 1, NULL, 0, &n) == MM_JOURNAL_UNCERTAIN);
        mm_journal_close(j);
        CHECK(mm_journal_open_volume(&j, path, &origin, &opt, false) == MM_JOURNAL_OK);
        uint64_t high = mm_journal_high(j);
        CHECK((high == 2 || high == 4) && mm_journal_prefix(j) == high);
        if (high == 2)
            CHECK(mm_journal_read(j, 1, NULL, 0, &n) == MM_JOURNAL_TOO_SMALL);
        else {
            CHECK(mm_journal_read(j, 1, NULL, 0, &n) == MM_JOURNAL_NOT_FOUND);
            CHECK(read_loss(j, 4).last == 1);
        }
        mm_journal_close(j);
        CHECK(unlink(path) == 0);
    }
}

static void protected_custody(const char *path)
{
    mm_journal_config c = origin;
    c.mode = MM_JOURNAL_RECEIVER;
    c.role = MM_CUSTODY;
    c.destination = 3;
    const mm_volume_options opt = {2, 512, MM_KEEP_NEW};
    mm_journal *j, *second;
    CHECK(mm_journal_open_volume(&j, path, &c, &opt, true) == MM_JOURNAL_OK);
    CHECK(mm_journal_open_volume(&second, path, &c, &opt, true) == MM_JOURNAL_BUSY);
    uint8_t frame[33] = {0};
    for (uint64_t seq = 1; seq <= 3; ++seq) {
        frame[32] = (uint8_t)seq;
        CHECK(mm_record_header(frame, MM_KIND_DATA, seq, 0, 1, frame + 32, 1) == MM_OK);
        CHECK(mm_journal_ingest(j, frame, sizeof(frame)) == (seq < 3 ? MM_JOURNAL_OK : MM_JOURNAL_FULL));
    }
    CHECK(mm_journal_missed(j) == 0 && mm_journal_reclaim(j) == MM_JOURNAL_OK);
    mm_journal_close(j);
    mm_journal_config wrong = c;
    ++wrong.stream.generation;
    CHECK(mm_journal_open_volume(&j, path, &wrong, &opt, true) == MM_JOURNAL_CONFLICT);
    CHECK(mm_journal_open_volume(&j, path, &c, &opt, false) == MM_JOURNAL_OK);
    for (uint64_t seq = 1; seq <= 2; ++seq) {
        size_t n;
        CHECK(mm_journal_read(j, seq, frame, sizeof(frame), &n) == MM_JOURNAL_OK && frame[32] == seq);
    }
    CHECK(mm_journal_prefix(j) == 2);
    mm_journal_close(j);
}

int main(void)
{
    char dir[] = "/tmp/mm-volume-XXXXXX";
    CHECK(mkdtemp(dir));
    char path[128];
    snprintf(path, sizeof(path), "%s/PIPE.MM", dir);
    group_survives_remount(path);
    CHECK(unlink(path) == 0);
    keep_old(path);
    CHECK(unlink(path) == 0);
    keep_new(path);
    CHECK(unlink(path) == 0);
    damaged_checkpoint(path);
    CHECK(unlink(path) == 0);
    interrupted_eviction(path);
    protected_custody(path);
    CHECK(unlink(path) == 0 && rmdir(dir) == 0);
    return 0;
}
