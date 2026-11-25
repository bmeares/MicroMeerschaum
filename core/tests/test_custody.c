// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include "micromeerschaum/peer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static const char key[] = "sensor\x1ftemp\x1f";
static const mm_stream stream = {1, 8, key, sizeof(key)-1};
static mm_journal *open_node(const char *path, mm_journal_mode mode, mm_receipt_role role,
                             uint64_t issuer, bool create)
{
    mm_journal *j;
    mm_journal_config config = {stream, mode, role, issuer, 9, 0};
    mm_volume_options opt = {4, 512, MM_KEEP_NEW};
    CHECK(mm_journal_open_volume(&j, path, &config, &opt, create) == MM_JOURNAL_OK);
    return j;
}

static size_t send(mm_journal *from, mm_journal *to, uint64_t sender, uint64_t after,
                   uint32_t count, uint8_t reply[MM_RECEIPT_MAX], uint64_t prefix)
{
    uint8_t batch[512];
    size_t n, reply_len;
    CHECK(mm_peer_batch(from, after, count, batch, sizeof batch, &n) == MM_JOURNAL_OK);
    CHECK(mm_peer_receive(to, sender, batch, n, reply, MM_RECEIPT_MAX, &reply_len) == MM_JOURNAL_OK);
    mm_receipt decoded;
    CHECK(mm_receipt_decode(reply, reply_len, &decoded) == MM_OK && decoded.through == prefix);
    return reply_len;
}

int main(void)
{
    char dir[] = "/tmp/mm-custody-XXXXXX";
    CHECK(mkdtemp(dir));
    char a[80], b[80], d[80];
    snprintf(a, sizeof a, "%s/a", dir);
    snprintf(b, sizeof b, "%s/b", dir);
    snprintf(d, sizeof d, "%s/d", dir);
    mm_journal *origin = open_node(a, MM_JOURNAL_ORIGIN, MM_CUSTODY, 2, true);
    mm_journal *relay = open_node(b, MM_JOURNAL_RECEIVER, MM_CUSTODY, 2, true);
    mm_journal *dest = open_node(d, MM_JOURNAL_RECEIVER, MM_DESTINATION, 9, true);
    uint8_t original[33], result[33], reply[MM_RECEIPT_MAX], stale[MM_RECEIPT_MAX];
    uint64_t seq;
    size_t n;
    for (uint8_t i = 1; i <= 3; ++i)
        CHECK(mm_journal_append(origin, MM_KIND_DATA, i, 3, &i, 1, &seq) == MM_JOURNAL_OK);
    CHECK(mm_journal_read(origin, 3, original, sizeof original, &n) == MM_JOURNAL_OK);
    send(origin, relay, 1, 0, 1, reply, 1); /* Lost custody reply. */
    mm_journal_close(relay);
    relay = open_node(b, MM_JOURNAL_RECEIVER, MM_CUSTODY, 2, false);
    CHECK(mm_journal_confirmed(origin) == 0);
    size_t stale_len = send(origin, relay, 1, 0, 1, stale, 1);
    send(origin, relay, 1, 2, 1, reply, 1);
    n = send(origin, relay, 1, 1, 1, reply, 3);
    CHECK(mm_peer_confirm(origin, 2, reply, n) == MM_JOURNAL_OK);
    CHECK(mm_peer_confirm(origin, 2, stale, stale_len) == MM_JOURNAL_OK);
    CHECK(mm_journal_confirmed(origin) == 3 && mm_journal_confirmed(relay) == 0);
    CHECK(mm_journal_reclaim(origin) == MM_JOURNAL_OK);
    /* Save a replay for after release, and a direct route to the final node. */
    uint8_t retry[512], direct[512];
    size_t retry_len, direct_len;
    CHECK(mm_peer_batch(relay, 0, 3, retry, sizeof retry, &retry_len) == MM_JOURNAL_OK);
    mm_v1_batch found;
    size_t h;
    uint64_t first, last;
    CHECK(mm_v1_batch_validate(retry, retry_len, &found, &h, &first, &last) == MM_OK);
    found.hop_count = 0; /* Same originals arrive via another authenticated link. */
    direct_len = mm_v1_batch_header(direct, sizeof direct, &found);
    memcpy(direct + direct_len, retry + h, retry_len - h);
    direct_len += retry_len - h;
    mm_journal_close(origin);
    CHECK(unlink(a) == 0); /* The destination has never met the origin. */
    mm_journal_close(relay);
    relay = open_node(b, MM_JOURNAL_RECEIVER, MM_CUSTODY, 2, false);
    CHECK(mm_peer_confirm(relay, 2, reply, n) != MM_JOURNAL_OK);
    CHECK(mm_journal_reclaim(relay) == MM_JOURNAL_OK);
    send(relay, dest, 2, 0, 3, reply, 3); /* Lost destination reply. */
    CHECK(mm_journal_read(relay, 3, result, sizeof result, &n) == MM_JOURNAL_OK);
    mm_journal_close(dest);
    dest = open_node(d, MM_JOURNAL_RECEIVER, MM_DESTINATION, 9, false);
    n = send(relay, dest, 2, 0, 3, reply, 3);
    CHECK(mm_peer_confirm(relay, 9, reply, n) == MM_JOURNAL_OK);
    CHECK(mm_journal_reclaim(relay) == MM_JOURNAL_OK);
    mm_journal_close(relay);
    relay = open_node(b, MM_JOURNAL_RECEIVER, MM_CUSTODY, 2, false);
    CHECK(mm_journal_prefix(relay) == 3 && mm_journal_confirmed(relay) == 3);
    CHECK(mm_journal_read(relay, 3, result, sizeof result, &n) == MM_JOURNAL_NOT_FOUND);
    CHECK(mm_journal_read(dest, 3, result, sizeof result, &n) == MM_JOURNAL_OK);
    CHECK(n == sizeof original && !memcmp(result, original, n));
    CHECK(mm_peer_receive(relay, 1, direct, direct_len, reply, sizeof reply, &n) == MM_JOURNAL_OK);
    CHECK(mm_journal_read(relay, 3, result, sizeof result, &n) == MM_JOURNAL_NOT_FOUND);
    CHECK(mm_peer_receive(dest, 1, direct, direct_len, reply, sizeof reply, &n) == MM_JOURNAL_OK);
    mm_journal_close(dest);
    dest = open_node(d, MM_JOURNAL_RECEIVER, MM_DESTINATION, 9, false);
    CHECK(mm_journal_route_count(dest) == 2);
    for (size_t i = 0; i < 2; ++i) {
        mm_route_range r;
        CHECK(mm_journal_route_get(dest, i, &r) == MM_JOURNAL_OK);
        CHECK(r.first == 1 && r.last == 3);
        CHECK(i ? (r.sender == 1 && !r.hop_count) :
                  (r.sender == 2 && r.hop_count == 1 && r.hops[0].node == 2));
    }
    mm_journal_close(relay);
    mm_journal_close(dest);
    CHECK(unlink(b) == 0 && unlink(d) == 0 && rmdir(dir) == 0);
}
