// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_PROTOCOL_H
#define MICROMEERSCHAUM_PROTOCOL_H

#include "micromeerschaum/mm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MM_PROTOCOL_VERSION 1u
#define MM_STREAM_MAX (32u + MM_KEY_MAX)
#define MM_V1_BATCH_HEADER_MAX (44u + MM_KEY_MAX + 24u * MM_BATCH_MAX_HOPS)
#define MM_RECEIPT_FIXED_SIZE 56u
#define MM_RECEIPT_MAX (MM_RECEIPT_FIXED_SIZE + MM_KEY_MAX)
#define MM_LOSS_SIZE 24u

/** Decoded keys borrow the encoded buffer and have an explicit byte length. */
typedef struct {
    uint64_t origin;
    uint64_t generation;
    const char *key;
    size_t key_len;
} mm_stream;

mm_status mm_stream_check(const mm_stream *stream);
int mm_stream_equal(const mm_stream *a, const mm_stream *b);
/** Encode or validate a complete saved stream identity. */
size_t mm_stream_encode(uint8_t *out, size_t cap, const mm_stream *stream);
mm_status mm_stream_decode(const uint8_t *buf, size_t len, mm_stream *stream);

typedef enum { MM_DESTINATION = 1, MM_CUSTODY = 2 } mm_receipt_role;

typedef struct {
    mm_stream stream;
    mm_receipt_role role;
    uint64_t issuer;
    uint64_t destination;
    uint64_t through;
} mm_receipt;

/** Encode a whole receipt, returning its size or zero on error. */
size_t mm_receipt_encode(uint8_t *out, size_t cap, const mm_receipt *receipt);
mm_status mm_receipt_decode(const uint8_t *buf, size_t len, mm_receipt *receipt);

typedef enum { MM_LOSS_MISSED = 1, MM_LOSS_EVICTED = 2 } mm_loss_kind;

typedef struct {
    mm_loss_kind kind;
    uint64_t first; /* missed count, or first evicted sequence (inclusive) */
    uint64_t last;  /* zero for missed observations, or last evicted sequence */
} mm_loss;

/** The declaration's own sequence must be greater than any evicted sequence. */
mm_status mm_loss_encode(uint8_t out[MM_LOSS_SIZE], const mm_loss *loss, uint64_t record_seq);
mm_status mm_loss_decode(const uint8_t *buf, size_t len, uint64_t record_seq, mm_loss *loss);

typedef struct {
    mm_stream stream;
    uint64_t destination;
    size_t hop_count;
    mm_hop hops[MM_BATCH_MAX_HOPS]; /* oldest first */
    uint32_t count;
} mm_v1_batch;

/** Write the header only. Append the original MR header and payload for each record. */
size_t mm_v1_batch_header(uint8_t *out, size_t cap, const mm_v1_batch *batch);

/** Validate a whole batch, including typed loss declarations. Empty batches yield range 0..0. */
mm_status mm_v1_batch_validate(const uint8_t *buf, size_t len, mm_v1_batch *batch,
                               size_t *header_size, uint64_t *first_seq, uint64_t *last_seq);

#ifdef __cplusplus
}
#endif
#endif
