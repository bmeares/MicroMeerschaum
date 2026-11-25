// SPDX-License-Identifier: Apache-2.0
#include <string.h>

#include "le.h"
#include "micromeerschaum/protocol.h"

#define HOP_BYTES 24u

mm_status mm_stream_check(const mm_stream *stream)
{
    if (!stream || stream->generation == 0) return MM_BAD_FIELD;
    if (!stream->key) return MM_BAD_KEY;
    return mm_key_check(stream->key, stream->key_len);
}

int mm_stream_equal(const mm_stream *a, const mm_stream *b)
{
    return a && b && a->origin == b->origin && a->generation == b->generation
        && a->key && b->key && a->key_len == b->key_len && a->key_len <= MM_KEY_MAX
        && memcmp(a->key, b->key, a->key_len) == 0;
}

static mm_status check_prefix(const uint8_t *buf, size_t len, size_t fixed, const char magic[4])
{
    if (len < fixed) return MM_TRUNCATED;
    if (!buf) return MM_BAD_FIELD;
    if (memcmp(buf, magic, 4) != 0) return MM_BAD_MAGIC;
    if (buf[4] != MM_PROTOCOL_VERSION) return MM_BAD_VERSION;
    return MM_OK;
}

/* fixed ends immediately after K. The CRC follows the key and any extra header fields. */
static mm_status check_header(const uint8_t *buf, size_t len, size_t fixed, size_t extra,
                              size_t *key_len, size_t *size)
{
    uint32_t k = mm_get32(buf + fixed - 4);
    if (k == 0 || k > MM_KEY_MAX) return MM_BAD_KEY;
    *key_len = (size_t)k;
    *size = fixed + *key_len + extra + 4;
    if (len < *size) return MM_TRUNCATED;
    if (mm_get32(buf + *size - 4) != mm_crc32(0, buf, *size - 4)) return MM_BAD_CRC;
    return MM_OK;
}

static mm_stream stream_view(const uint8_t *buf, size_t fixed, size_t key_len)
{
    mm_stream stream = {mm_get64(buf + 8), mm_get64(buf + 16), (const char *)buf + fixed, key_len};
    return stream;
}

static void write_header(uint8_t *out, const char magic[4], uint8_t tag,
                         size_t fixed, const mm_stream *stream)
{
    memcpy(out, magic, 4);
    out[4] = MM_PROTOCOL_VERSION;
    out[5] = tag;
    out[6] = out[7] = 0;
    mm_put64(out + 8, stream->origin);
    mm_put64(out + 16, stream->generation);
    mm_put32(out + fixed - 4, (uint32_t)stream->key_len);
    memcpy(out + fixed, stream->key, stream->key_len);
}

static void write_crc(uint8_t *out, size_t size)
{
    mm_put32(out + size - 4, mm_crc32(0, out, size - 4));
}

size_t mm_stream_encode(uint8_t *out, size_t cap, const mm_stream *stream)
{
    if (mm_stream_check(stream) != MM_OK) return 0;
    size_t size = 32 + stream->key_len;
    if (!out || cap < size) return 0;
    write_header(out, "MMID", 0, 28, stream);
    write_crc(out, size);
    return size;
}

mm_status mm_stream_decode(const uint8_t *buf, size_t len, mm_stream *stream)
{
    mm_status status = check_prefix(buf, len, 28, "MMID");
    if (status != MM_OK) return status;
    if (!stream || !mm_all_zero(buf + 5, 3)) return MM_BAD_FIELD;
    size_t k, size;
    status = check_header(buf, len, 28, 0, &k, &size);
    if (status != MM_OK) return status;
    mm_stream result = stream_view(buf, 28, k);
    status = mm_stream_check(&result);
    if (status != MM_OK) return status;
    if (size != len) return MM_TRAILING;
    *stream = result;
    return MM_OK;
}

static int valid_role(mm_receipt_role role)
{
    return role == MM_DESTINATION || role == MM_CUSTODY;
}

static int valid_issuer(const mm_receipt *receipt)
{
    return (receipt->issuer == receipt->destination) == (receipt->role == MM_DESTINATION);
}

size_t mm_receipt_encode(uint8_t *out, size_t cap, const mm_receipt *receipt)
{
    if (!receipt || mm_stream_check(&receipt->stream) != MM_OK
        || !valid_role(receipt->role) || !valid_issuer(receipt)) return 0;
    size_t size = MM_RECEIPT_FIXED_SIZE + receipt->stream.key_len;
    if (!out || cap < size) return 0;
    write_header(out, "MMRC", (uint8_t)receipt->role, 52, &receipt->stream);
    mm_put64(out + 24, receipt->issuer);
    mm_put64(out + 32, receipt->destination);
    mm_put64(out + 40, receipt->through);
    write_crc(out, size);
    return size;
}

mm_status mm_receipt_decode(const uint8_t *buf, size_t len, mm_receipt *receipt)
{
    mm_status status = check_prefix(buf, len, 52, "MMRC");
    if (status != MM_OK) return status;
    mm_receipt r;
    r.role = (mm_receipt_role)buf[5];
    if (!receipt || !valid_role(r.role) || !mm_all_zero(buf + 6, 2)) return MM_BAD_FIELD;
    size_t k, size;
    status = check_header(buf, len, 52, 0, &k, &size);
    if (status != MM_OK) return status;
    r.stream = stream_view(buf, 52, k);
    r.issuer = mm_get64(buf + 24);
    r.destination = mm_get64(buf + 32);
    r.through = mm_get64(buf + 40);
    if (!valid_issuer(&r)) return MM_BAD_FIELD;
    status = mm_stream_check(&r.stream);
    if (status != MM_OK) return status;
    if (len != size) return MM_TRAILING;
    *receipt = r;
    return MM_OK;
}

static mm_status check_loss(const mm_loss *loss, uint64_t seq)
{
    if (!loss || (loss->kind != MM_LOSS_MISSED && loss->kind != MM_LOSS_EVICTED))
        return MM_BAD_FIELD;
    if (seq == 0) return MM_BAD_SEQ;
    if (loss->first == 0) return MM_BAD_FIELD;
    if (loss->kind == MM_LOSS_MISSED) return loss->last == 0 ? MM_OK : MM_BAD_FIELD;
    return loss->first <= loss->last && loss->last < seq ? MM_OK : MM_BAD_FIELD;
}

mm_status mm_loss_encode(uint8_t out[MM_LOSS_SIZE], const mm_loss *loss, uint64_t record_seq)
{
    mm_status status = check_loss(loss, record_seq);
    if (status != MM_OK) return status;
    if (!out) return MM_BAD_FIELD;
    out[0] = (uint8_t)loss->kind;
    memset(out + 1, 0, 7);
    mm_put64(out + 8, loss->first);
    mm_put64(out + 16, loss->last);
    return MM_OK;
}

mm_status mm_loss_decode(const uint8_t *buf, size_t len, uint64_t record_seq, mm_loss *loss)
{
    if (len < MM_LOSS_SIZE) return MM_TRUNCATED;
    if (len > MM_LOSS_SIZE) return MM_TRAILING;
    if (!buf || !loss || !mm_all_zero(buf + 1, 7)) return MM_BAD_FIELD;
    mm_loss result = {(mm_loss_kind)buf[0], mm_get64(buf + 8), mm_get64(buf + 16)};
    mm_status status = check_loss(&result, record_seq);
    if (status == MM_OK) *loss = result;
    return status;
}

size_t mm_v1_batch_header(uint8_t *out, size_t cap, const mm_v1_batch *batch)
{
    if (!batch || mm_stream_check(&batch->stream) != MM_OK || batch->hop_count > MM_BATCH_MAX_HOPS)
        return 0;
    for (size_t i = 0; i < batch->hop_count; i++) {
        if ((unsigned)batch->hops[i].transport > MM_TRANSPORT_USB) return 0;
    }
    size_t size = 44 + batch->stream.key_len + HOP_BYTES * batch->hop_count;
    if (!out || cap < size) return 0;
    write_header(out, "MMBT", (uint8_t)batch->hop_count, 36, &batch->stream);
    mm_put64(out + 24, batch->destination);
    uint8_t *p = out + 36 + batch->stream.key_len;
    for (size_t i = 0; i < batch->hop_count; i++, p += HOP_BYTES) {
        mm_put64(p, batch->hops[i].node);
        mm_put64(p + 8, (uint64_t)batch->hops[i].received_us);
        p[16] = (uint8_t)batch->hops[i].transport;
        memset(p + 17, 0, 7);
    }
    mm_put32(p, batch->count);
    write_crc(out, size);
    return size;
}

mm_status mm_v1_batch_validate(const uint8_t *buf, size_t len, mm_v1_batch *batch,
                               size_t *header_size, uint64_t *first_seq, uint64_t *last_seq)
{
    mm_status status = check_prefix(buf, len, 36, "MMBT");
    if (status != MM_OK) return status;
    if (!batch || !header_size || !first_seq || !last_seq) return MM_BAD_FIELD;
    mm_v1_batch result = {0};
    result.hop_count = buf[5];
    if (result.hop_count > MM_BATCH_MAX_HOPS || !mm_all_zero(buf + 6, 2)) return MM_BAD_FIELD;
    size_t k, size;
    status = check_header(buf, len, 36, 4 + result.hop_count * HOP_BYTES, &k, &size);
    if (status != MM_OK) return status;
    result.stream = stream_view(buf, 36, k);
    status = mm_stream_check(&result.stream);
    if (status != MM_OK) return status;
    result.destination = mm_get64(buf + 24);
    const uint8_t *p = buf + 36 + k;
    for (size_t i = 0; i < result.hop_count; i++, p += HOP_BYTES) {
        if (p[16] > MM_TRANSPORT_USB || !mm_all_zero(p + 17, 7)) return MM_BAD_FIELD;
        result.hops[i].node = mm_get64(p);
        result.hops[i].received_us = mm_get_i64(p + 8);
        result.hops[i].transport = (mm_transport)p[16];
    }
    result.count = mm_get32(p);
    size_t offset = size;
    uint64_t first = 0, last = 0;
    for (uint32_t i = 0; i < result.count; i++) {
        mm_record record;
        size_t used;
        status = mm_record_decode(buf + offset, len - offset, &record, &used);
        if (status != MM_OK) return status;
        if (i == 0) first = record.seq;
        else if (last == UINT64_MAX || record.seq != last + 1) return MM_SEQ_GAP;
        if (record.kind == MM_KIND_LOSS) {
            mm_loss loss;
            status = mm_loss_decode(record.payload, record.len, record.seq, &loss);
            if (status != MM_OK) return status;
        }
        offset += used;
        last = record.seq;
    }
    if (offset != len) return MM_TRAILING;
    *batch = result;
    *header_size = size;
    *first_seq = first;
    *last_seq = last;
    return MM_OK;
}
