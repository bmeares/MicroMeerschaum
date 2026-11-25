// SPDX-License-Identifier: Apache-2.0
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "micromeerschaum/peer.h"

/* A provisioned two-node link, with text samples as the opaque payload. */
static const char key[] = "sensor\x1f" "reading\x1f";
static const mm_journal_config origin_config = {
    .stream = {.origin = 1, .generation = 1, .key = key, .key_len = sizeof key - 1},
    .mode = MM_JOURNAL_ORIGIN, .role = MM_DESTINATION, .issuer = 2, .destination = 2
};

static mm_journal_status send_backlog(mm_journal *origin, mm_journal *peer)
{
    static uint8_t batch[MM_V1_BATCH_HEADER_MAX + MM_RECORD_HEADER_SIZE + MM_RECORD_MAX_PAYLOAD];
    uint8_t reply[MM_RECEIPT_MAX];
    while (mm_journal_confirmed(origin) < mm_journal_high(origin)) {
        uint64_t after = mm_journal_confirmed(origin);
        size_t batch_len, reply_len;
        mm_journal_status status = mm_peer_batch(origin, after, UINT32_MAX, batch, sizeof batch,
                                                 &batch_len);
        if (status != MM_JOURNAL_OK) return status;
        status = mm_peer_receive(peer, origin_config.stream.origin, batch, batch_len,
                                 reply, sizeof reply, &reply_len);
        if (status != MM_JOURNAL_OK) return status;
        status = mm_peer_confirm(origin, origin_config.issuer, reply, reply_len);
        if (status != MM_JOURNAL_OK) return status;
        if (mm_journal_confirmed(origin) <= after) return MM_JOURNAL_CONFLICT;
    }
    return mm_journal_reclaim(origin);
}

int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[1], "collect") != 0 && strcmp(argv[1], "send") != 0)) {
        fprintf(stderr, "Usage: %s collect ORIGIN_DIR READING\n"
                        "       %s send ORIGIN_DIR PEER_DIR\n", argv[0], argv[0]);
        return 2;
    }
    bool collect = strcmp(argv[1], "collect") == 0;
    mm_journal *origin = NULL, *peer = NULL;
    mm_journal_status status = mm_journal_open(&origin, argv[2], &origin_config, collect);
    if (status == MM_JOURNAL_OK && collect) {
        size_t len = strlen(argv[3]);
        uint64_t seq = 0;
        status = len > MM_RECORD_MAX_PAYLOAD ? MM_JOURNAL_INVALID :
            mm_journal_append(origin, MM_KIND_DATA, 0, 0, argv[3], (uint32_t)len, &seq);
        if (status == MM_JOURNAL_OK) printf("Committed locally through %" PRIu64 "\n", seq);
        if (status == MM_JOURNAL_UNCERTAIN)
            fprintf(stderr, "Reopen and check sequence %" PRIu64 " before retrying\n", seq);
    } else if (status == MM_JOURNAL_OK) {
        mm_journal_config receiver_config = origin_config;
        receiver_config.mode = MM_JOURNAL_RECEIVER;
        status = mm_journal_open(&peer, argv[3], &receiver_config, true);
        if (status == MM_JOURNAL_OK) status = send_backlog(origin, peer);
        if (status == MM_JOURNAL_OK)
            printf("Peer confirmed through %" PRIu64 "\n", mm_journal_confirmed(origin));
    }
    if (peer) mm_journal_close(peer);
    if (origin) mm_journal_close(origin);
    if (status != MM_JOURNAL_OK) fprintf(stderr, "%s: %s\n", argv[1], mm_journal_status_name(status));
    return status == MM_JOURNAL_OK ? 0 : 1;
}
