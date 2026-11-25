// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/peer.h"

mm_journal_status mm_peer_batch(mm_journal *origin, uint64_t after, uint32_t max_records,
                                uint8_t *out, size_t cap, size_t *used)
{
    if (!used) return MM_JOURNAL_INVALID;
    *used = 0;
    if (!origin || !out) return MM_JOURNAL_INVALID;
    if (mm_journal_needs_reopen(origin)) return MM_JOURNAL_UNCERTAIN;
    const mm_journal_config *config = mm_journal_config_get(origin);
    if (config->mode != MM_JOURNAL_ORIGIN || config->role != MM_DESTINATION)
        return MM_JOURNAL_INVALID;

    uint64_t high = mm_journal_high(origin);
    if (after > high) return MM_JOURNAL_INVALID;
    mm_v1_batch batch = {.stream = config->stream, .destination = config->destination};
    size_t offset = mm_v1_batch_header(out, cap, &batch);
    if (!offset) return MM_JOURNAL_TOO_SMALL;
    /* Every emitted batch must fit one receiver commit, even if the caller
     * supplied a much larger radio or file buffer. */
    const size_t max_records_bytes = MM_JOURNAL_SEGMENT_MAX - MM_SEGMENT_HEADER_MAX;
    if (cap - offset > max_records_bytes) cap = offset + max_records_bytes;

    if (max_records && after < high) {
        uint64_t start;
        mm_journal_status st = mm_journal_delivery_start(origin, after + 1, &start);
        if (st != MM_JOURNAL_OK) return st;
        after = start - 1;
    }
    while (after < high && batch.count < max_records) {
        size_t record_len = 0;
        mm_journal_status status = mm_journal_read(origin, after + 1, out + offset,
                                                   cap - offset, &record_len);
        if ((status == MM_JOURNAL_TOO_SMALL || status == MM_JOURNAL_NOT_FOUND) && batch.count) break;
        if (status != MM_JOURNAL_OK) return status;
        offset += record_len;
        batch.count++;
        after++;
    }
    if (batch.count) mm_v1_batch_header(out, cap, &batch);
    *used = offset;
    return MM_JOURNAL_OK;
}

mm_journal_status mm_peer_receive(mm_journal *receiver, uint64_t authenticated_origin,
                                  const uint8_t *data, size_t len, uint8_t *reply,
                                  size_t reply_cap, size_t *reply_len)
{
    if (!reply_len) return MM_JOURNAL_INVALID;
    *reply_len = 0;
    if (!receiver || !data || !reply) return MM_JOURNAL_INVALID;
    const mm_journal_config *config = mm_journal_config_get(receiver);
    if (config->mode != MM_JOURNAL_RECEIVER || config->role != MM_DESTINATION ||
        authenticated_origin != config->stream.origin) return MM_JOURNAL_INVALID;

    mm_v1_batch batch;
    size_t header_len;
    uint64_t first, last;
    if (mm_v1_batch_validate(data, len, &batch, &header_len, &first, &last) != MM_OK ||
        !mm_stream_equal(&batch.stream, &config->stream) ||
        batch.destination != config->destination || batch.hop_count != 0)
        return MM_JOURNAL_INVALID;
    if (reply_cap < MM_RECEIPT_FIXED_SIZE + config->stream.key_len) return MM_JOURNAL_TOO_SMALL;

    mm_journal_status status = mm_journal_ingest(receiver, data + header_len, len - header_len);
    if (status != MM_JOURNAL_OK) return status;
    mm_receipt receipt = {
        .stream = config->stream, .role = MM_DESTINATION, .issuer = config->issuer,
        .destination = config->destination, .through = mm_journal_prefix(receiver)
    };
    *reply_len = mm_receipt_encode(reply, reply_cap, &receipt);
    return *reply_len ? MM_JOURNAL_OK : MM_JOURNAL_INVALID;
}

mm_journal_status mm_peer_confirm(mm_journal *origin, uint64_t authenticated_issuer,
                                  const uint8_t *reply, size_t len)
{
    if (!origin || !reply) return MM_JOURNAL_INVALID;
    const mm_journal_config *config = mm_journal_config_get(origin);
    if (config->mode != MM_JOURNAL_ORIGIN || config->role != MM_DESTINATION ||
        authenticated_issuer != config->issuer) return MM_JOURNAL_INVALID;
    mm_receipt receipt;
    if (mm_receipt_decode(reply, len, &receipt) != MM_OK) return MM_JOURNAL_INVALID;
    return mm_journal_confirm(origin, &receipt);
}
