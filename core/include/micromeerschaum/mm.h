// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_MM_H
#define MICROMEERSCHAUM_MM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Wire-format version this core implements (spec/wire-format.md). */
#define MM_WIRE_VERSION 0u

#define MM_RECORD_HEADER_SIZE 32u
#define MM_RECORD_MAX_PAYLOAD 65536u
#define MM_KEY_MAX 200u
#define MM_BATCH_MAX_HOPS 8u
#define MM_SEGMENT_HEADER_MAX (28u + MM_KEY_MAX)
#define MM_BATCH_HEADER_MAX (28u + MM_KEY_MAX + 24u * MM_BATCH_MAX_HOPS)

/** Decode results; mm_status_name() gives the spec's names. */
typedef enum {
    MM_OK = 0,
    MM_END,       /* scan: the data ends exactly after a record */
    MM_TRUNCATED, /* the input ends inside the structure: more bytes are needed */
    MM_BAD_MAGIC,
    MM_BAD_VERSION,
    MM_BAD_KIND,
    MM_BAD_LENGTH,
    MM_BAD_SEQ,
    MM_SEQ_GAP,
    MM_BAD_KEY,
    MM_BAD_FIELD,
    MM_BAD_CRC,
    MM_TRAILING,
} mm_status;

typedef enum { MM_KIND_DATA = 0, MM_KIND_LOSS = 1 } mm_kind;

typedef enum {
    MM_TRANSPORT_LOCAL = 0,
    MM_TRANSPORT_BLE = 1,
    MM_TRANSPORT_WIFI = 2,
    MM_TRANSPORT_LTE = 3,
    MM_TRANSPORT_USB = 4,
} mm_transport;

/** "ok", "bad_crc", ... as in the spec; "?" for an unknown value. */
const char *mm_status_name(mm_status status);

/**
 * CRC-32/ISO-HDLC, chainable like zlib's crc32(): start with 0 and pass the
 * previous result to continue over the next chunk.
 */
uint32_t mm_crc32(uint32_t crc, const void *data, size_t len);

/** MM_OK if key is a valid device pipe key, else MM_BAD_KEY. */
mm_status mm_key_check(const char *key, size_t len);

/**
 * Writes the 32-byte header of a record whose len payload bytes are stored
 * elsewhere; the record is this header followed by the payload. Writes nothing
 * and returns MM_BAD_KIND, MM_BAD_LENGTH or MM_BAD_SEQ (seq 0) on bad input.
 */
mm_status mm_record_header(uint8_t out[MM_RECORD_HEADER_SIZE], mm_kind kind, uint64_t seq,
                           uint64_t mono_us, uint32_t boot, const void *payload, uint32_t len);

typedef struct {
    mm_kind kind;
    uint64_t seq;
    uint64_t mono_us;
    uint32_t boot;
    uint32_t len;
    const uint8_t *payload; /* points into the decoded buffer */
} mm_record;

/**
 * Decodes the record at the start of buf. MM_OK sets *rec and *consumed
 * (32 + payload length); MM_TRUNCATED means more bytes are needed; any other
 * status means the bytes are not a valid record.
 */
mm_status mm_record_decode(const uint8_t *buf, size_t len, mm_record *rec, size_t *consumed);

/**
 * Writes a segment header; returns its size (28 + key_len), or 0 if cap is too
 * small, the key is invalid or first_seq is 0.
 */
size_t mm_segment_header(uint8_t *out, size_t cap, const char *key, size_t key_len,
                         uint64_t first_seq, uint32_t boot);

typedef struct {
    size_t header_size; /* bytes before the first record */
    const char *key;    /* points into the scanned buffer; not NUL-terminated */
    size_t key_len;
    uint64_t first_seq;
    uint32_t boot;
    uint64_t records;
    uint64_t last_seq;  /* first_seq + records - 1 */
    size_t frontier;    /* offset of the first byte after the last valid record */
    mm_status stop;     /* MM_END, MM_SEQ_GAP or the status of the record at the frontier */
} mm_scan;

/**
 * Decodes the segment header at the start of data and scans the records after
 * it. Returns the header's status; *out is set only on MM_OK.
 */
mm_status mm_segment_scan(const uint8_t *data, size_t len, mm_scan *out);

typedef struct {
    uint64_t node;
    int64_t received_us; /* Unix microseconds */
    mm_transport transport;
} mm_hop;

typedef struct {
    uint64_t origin;
    const char *key; /* decoded: points into the buffer; not NUL-terminated */
    size_t key_len;
    size_t hop_count;
    mm_hop hops[MM_BATCH_MAX_HOPS]; /* oldest first */
    uint32_t count;                 /* records after the header */
} mm_batch;

/**
 * Writes a batch header; returns its size (28 + key_len + 24 * hop_count), or
 * 0 if cap is too small or a field is invalid.
 */
size_t mm_batch_header(uint8_t *out, size_t cap, const mm_batch *batch);

/** Decodes the batch header at the start of buf; MM_OK sets *batch and *header_size. */
mm_status mm_batch_header_decode(const uint8_t *buf, size_t len, mm_batch *batch,
                                 size_t *header_size);

/**
 * Validates a whole batch: its header, then exactly batch->count records with
 * consecutive seq. MM_OK sets *batch and the seq range (both 0 when count is 0).
 */
mm_status mm_batch_validate(const uint8_t *buf, size_t len, mm_batch *batch,
                            uint64_t *first_seq, uint64_t *last_seq);

#ifdef __cplusplus
}
#endif

#endif
