// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "micromeerschaum/peer.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

static const char key[] = "sensor\x1f" "temperature\x1f";
static const mm_journal_config stream = {
    .stream = {.origin = 7, .generation = 12, .key = key, .key_len = sizeof key - 1},
    .mode = MM_JOURNAL_ORIGIN, .role = MM_DESTINATION, .issuer = 17, .destination = 17
};

typedef struct {
    char dir[64];
    mm_journal_config config;
    mm_journal *journal;
} node;

static void provision(node *n, mm_journal_config config)
{
    strcpy(n->dir, "/tmp/mm-delivery-XXXXXX");
    CHECK(mkdtemp(n->dir) != NULL);
    n->config = config;
    CHECK(mm_journal_open(&n->journal, n->dir, &n->config, true) == MM_JOURNAL_OK);
}

static void reopen_node(node *n)
{
    mm_journal_close(n->journal);
    CHECK(mm_journal_open(&n->journal, n->dir, &n->config, false) == MM_JOURNAL_OK);
}

static void remove_node(node *n)
{
    mm_journal_close(n->journal);
    DIR *d = opendir(n->dir);
    CHECK(d != NULL);
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        CHECK(unlinkat(dirfd(d), entry->d_name, 0) == 0);
    }
    CHECK(closedir(d) == 0);
    CHECK(rmdir(n->dir) == 0);
}

static void add_reading(node *n, uint64_t expected_seq)
{
    /* Equal measurements at distinct sequence numbers are intentional. */
    uint64_t seq;
    CHECK(mm_journal_append(n->journal, MM_KIND_DATA, 100, 2, "21.4", 4, &seq) == MM_JOURNAL_OK);
    CHECK(seq == expected_seq);
}

static uint8_t batch[1024], reply[MM_RECEIPT_MAX];
static size_t batch_len, reply_len;

static void prepare(node *a, uint64_t after, uint32_t count)
{
    CHECK(mm_peer_batch(a->journal, after, count, batch, sizeof batch, &batch_len) == MM_JOURNAL_OK);
}

static void accept(node *b, uint64_t expected_prefix)
{
    CHECK(mm_peer_receive(b->journal, stream.stream.origin, batch, batch_len, reply,
                          sizeof reply, &reply_len) == MM_JOURNAL_OK);
    mm_receipt receipt;
    CHECK(mm_receipt_decode(reply, reply_len, &receipt) == MM_OK);
    CHECK(receipt.through == expected_prefix && mm_journal_prefix(b->journal) == expected_prefix);
}

static void confirm(node *a)
{
    CHECK(mm_peer_confirm(a->journal, stream.issuer, reply, reply_len) == MM_JOURNAL_OK);
}

static int fail_directory_sync(int fd, void *context)
{
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        *(bool *)context = true;
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static void stale_receipts(const uint8_t *old_reply, size_t old_size)
{
    for (unsigned change = 0; change < 2; ++change) {
        mm_journal_config config = stream;
        if (change == 0) config.stream.generation++;
        else {
            config.destination++;
            config.issuer = config.destination;
        }
        node fresh;
        provision(&fresh, config);
        add_reading(&fresh, 1);
        CHECK(mm_peer_confirm(fresh.journal, config.issuer, old_reply, old_size) != MM_JOURNAL_OK);
        CHECK(mm_journal_confirmed(fresh.journal) == 0);
        remove_node(&fresh);
    }
}

static void delivery(void)
{
    node a, b;
    provision(&a, stream);
    mm_journal_config target = stream;
    target.mode = MM_JOURNAL_RECEIVER;
    provision(&b, target);
    for (uint64_t i = 1; i <= 3; ++i) add_reading(&a, i);

    /* A byte budget can fit one complete record, even when several were requested. */
    prepare(&a, 0, 1);
    size_t one = batch_len;
    CHECK(mm_peer_batch(a.journal, 0, 3, batch, one - 1, &batch_len) == MM_JOURNAL_TOO_SMALL);
    CHECK(batch_len == 0);
    CHECK(mm_peer_batch(a.journal, 0, 3, batch, one, &batch_len) == MM_JOURNAL_OK);
    CHECK(batch_len == one);
    CHECK(mm_peer_receive(b.journal, 88, batch, batch_len, reply, sizeof reply,
                          &reply_len) == MM_JOURNAL_INVALID);
    CHECK(mm_peer_receive(b.journal, stream.stream.origin, batch, batch_len, reply, 1,
                          &reply_len) == MM_JOURNAL_TOO_SMALL);
    CHECK(reply_len == 0 && mm_journal_prefix(b.journal) == 0);
    /* Commit in a separate receiver process and exit without returning the
     * reply or closing the handle through the library. */
    mm_journal_close(b.journal);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        CHECK(mm_journal_open(&b.journal, b.dir, &b.config, false) == MM_JOURNAL_OK);
        accept(&b, 1);
        _exit(0);
    }
    int child_status;
    CHECK(waitpid(child, &child_status, 0) == child);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    reopen_node(&a);
    CHECK(mm_journal_open(&b.journal, b.dir, &b.config, false) == MM_JOURNAL_OK);
    CHECK(mm_journal_confirmed(a.journal) == 0);
    prepare(&a, 0, 1);
    accept(&b, 1);
    CHECK(mm_peer_confirm(a.journal, 88, reply, reply_len) == MM_JOURNAL_INVALID);
    stale_receipts(reply, reply_len);
    confirm(&a);
    prepare(&a, 2, 1);
    accept(&b, 1);     /* Record 3 is durable while 2 is missing. */
    reopen_node(&b);
    prepare(&a, 1, 0);
    accept(&b, 1);     /* A receipt query reports the recovered prefix. */
    prepare(&a, 1, 1);
    accept(&b, 3);
    confirm(&a);

    for (uint64_t i = 1; i <= 3; ++i) {
        uint8_t original[128], received[128];
        size_t original_len, received_len;
        CHECK(mm_journal_read(a.journal, i, original, sizeof original, &original_len) == MM_JOURNAL_OK);
        CHECK(mm_journal_read(b.journal, i, received, sizeof received, &received_len) == MM_JOURNAL_OK);
        CHECK(original_len == received_len && memcmp(original, received, original_len) == 0);
    }

    /* A syntactically valid record cannot replace the bytes for a known identity. */
    prepare(&a, 1, 1);
    mm_v1_batch decoded;
    size_t header;
    uint64_t first, last;
    CHECK(mm_v1_batch_validate(batch, batch_len, &decoded, &header, &first, &last) == MM_OK);
    CHECK(first == 2 && last == 2);
    uint8_t *payload = batch + header + MM_RECORD_HEADER_SIZE;
    payload[0] ^= 1;
    CHECK(mm_record_header(batch + header, MM_KIND_DATA, 2, 100, 2, payload, 4) == MM_OK);
    CHECK(mm_peer_receive(b.journal, stream.stream.origin, batch, batch_len, reply,
                          sizeof reply, &reply_len) == MM_JOURNAL_CONFLICT);
    CHECK(reply_len == 0 && mm_journal_prefix(b.journal) == 3);

    CHECK(mm_journal_reclaim(a.journal) == MM_JOURNAL_OK);
    reopen_node(&a);
    CHECK(mm_journal_confirmed(a.journal) == 3);
    add_reading(&a, 4);
    prepare(&a, 3, 1);
    bool failed = false;
    mm_journal_set_barrier(b.journal, fail_directory_sync, &failed);
    CHECK(mm_peer_receive(b.journal, stream.stream.origin, batch, batch_len, reply,
                          sizeof reply, &reply_len) == MM_JOURNAL_UNCERTAIN);
    CHECK(failed && reply_len == 0 && mm_journal_confirmed(a.journal) == 3);
    prepare(&a, 3, 0);
    CHECK(mm_peer_receive(b.journal, stream.stream.origin, batch, batch_len, reply,
                          sizeof reply, &reply_len) == MM_JOURNAL_UNCERTAIN);
    CHECK(reply_len == 0);
    reopen_node(&b);
    prepare(&a, 3, 1);
    accept(&b, 4);
    failed = false;
    mm_journal_set_barrier(a.journal, fail_directory_sync, &failed);
    CHECK(mm_peer_confirm(a.journal, stream.issuer, reply, reply_len) == MM_JOURNAL_UNCERTAIN);
    CHECK(failed && mm_journal_reclaim(a.journal) == MM_JOURNAL_UNCERTAIN);
    CHECK(mm_peer_batch(a.journal, 3, 0, batch, sizeof batch, &batch_len) == MM_JOURNAL_UNCERTAIN);
    CHECK(batch_len == 0);
    reopen_node(&a);
    confirm(&a);
    remove_node(&a);
    remove_node(&b);
}

static void declared_loss(void)
{
    node a, b;
    provision(&a, stream);
    mm_journal_config target = stream;
    target.mode = MM_JOURNAL_RECEIVER;
    provision(&b, target);
    add_reading(&a, 1);
    add_reading(&a, 2);

    uint8_t payload[MM_LOSS_SIZE];
    uint64_t seq;
    mm_loss missed = {.kind = MM_LOSS_MISSED, .first = 5};
    CHECK(mm_loss_encode(payload, &missed, 3) == MM_OK);
    CHECK(mm_journal_append(a.journal, MM_KIND_LOSS, 105, 2, payload, sizeof payload, &seq)
          == MM_JOURNAL_OK && seq == 3);
    mm_loss evicted = {.kind = MM_LOSS_EVICTED, .first = 2, .last = 2};
    CHECK(mm_loss_encode(payload, &evicted, 4) == MM_OK);
    CHECK(mm_journal_append(a.journal, MM_KIND_LOSS, 110, 2, payload, sizeof payload, &seq)
          == MM_JOURNAL_OK && seq == 4);
    prepare(&a, 0, 1);
    accept(&b, 1);
    prepare(&a, 2, 1);
    accept(&b, 1);  /* Five never-admitted samples cannot cover sequence 2. */
    prepare(&a, 3, 1);
    accept(&b, 4);
    reopen_node(&b);
    size_t used;
    CHECK(mm_journal_read(b.journal, 2, batch, sizeof batch, &used) == MM_JOURNAL_NOT_FOUND);
    prepare(&a, 0, 0);
    accept(&b, 4);
    confirm(&a);
    remove_node(&a);
    remove_node(&b);
}

static int count_syncs(int fd, void *context)
{
    ++*(unsigned *)context;
    return fsync(fd);
}

static void grouped_transfer(void)
{
    node a, b;
    provision(&a, stream);
    mm_journal_config target = stream;
    target.mode = MM_JOURNAL_RECEIVER;
    provision(&b, target);
    unsigned local_syncs = 0, peer_syncs = 0;
    mm_journal_set_barrier(a.journal, count_syncs, &local_syncs);
    mm_journal_set_barrier(b.journal, count_syncs, &peer_syncs);
    mm_sample group[12];
    for (size_t i = 0; i < sizeof group / sizeof group[0]; ++i)
        group[i] = (mm_sample){.kind = MM_KIND_DATA, .mono_us = i, .payload = "x", .len = 1};
    uint64_t first;
    CHECK(mm_journal_append_group(a.journal, group, 12, &first) == MM_JOURNAL_OK && first == 1);
    CHECK(local_syncs == 2);  /* One file and one directory sync for all twelve readings. */
    prepare(&a, 0, 12);
    accept(&b, 12);
    CHECK(peer_syncs == 4);
    accept(&b, 12);
    CHECK(peer_syncs == 4);   /* A retry reuses the committed segment. */
    confirm(&a);
    CHECK(mm_journal_confirmed(a.journal) == 12);
    remove_node(&a);
    remove_node(&b);
}

int main(void)
{
    delivery();
    declared_loss();
    grouped_transfer();
    return 0;
}
