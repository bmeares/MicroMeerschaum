// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_JOURNAL_H
#define MICROMEERSCHAUM_JOURNAL_H

#include <stdbool.h>
#include "micromeerschaum/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* POSIX reference port. Provision one private, durably linked directory per
 * store. The handle holds an exclusive lock until closed. Serialize calls on
 * it. Memory is bounded per segment, with one descriptor per published run
 * and one interval per distinct coverage range. */
typedef struct mm_journal mm_journal;

typedef enum {
    MM_JOURNAL_OK = 0, MM_JOURNAL_INVALID, MM_JOURNAL_IO, MM_JOURNAL_CORRUPT,
    MM_JOURNAL_CONFLICT, MM_JOURNAL_NOT_FOUND, MM_JOURNAL_TOO_SMALL,
    MM_JOURNAL_FULL, MM_JOURNAL_BUSY, MM_JOURNAL_UNCERTAIN, MM_JOURNAL_NOMEM
} mm_journal_status;
const char *mm_journal_status_name(mm_journal_status status);

typedef enum { MM_JOURNAL_ORIGIN = 1, MM_JOURNAL_RECEIVER = 2 } mm_journal_mode;

typedef struct {
    mm_stream stream;
    mm_journal_mode mode;
    mm_receipt_role role;
    uint64_t issuer;       /* Authorized receiver of this delivery. */
    uint64_t destination;  /* Final destination, including for custody. */
    uint64_t max_bytes;    /* Runtime limit on published segment bytes. Zero is unlimited. */
} mm_journal_config;

#define MM_JOURNAL_SEGMENT_MAX (1024u * 1024u)

typedef struct {
    mm_kind kind;
    uint64_t mono_us;
    uint32_t boot;
    const void *payload;
    uint32_t len;
} mm_sample;

/* create=true provisions an empty directory or reopens a matching store.
 * create=false requires a provisioned store. Never formats existing contents. */
mm_journal_status mm_journal_open(mm_journal **out, const char *directory,
                                  const mm_journal_config *config, bool create);
void mm_journal_close(mm_journal *j);
const mm_journal_config *mm_journal_config_get(const mm_journal *j);
/* A failed/uncertain commit requires reopening before reading or issuing receipts. */
bool mm_journal_needs_reopen(const mm_journal *j);
uint64_t mm_journal_prefix(const mm_journal *j);     /* Contiguous accounted-for prefix. */
uint64_t mm_journal_high(const mm_journal *j);       /* Highest allocated or received sequence. */
uint64_t mm_journal_confirmed(const mm_journal *j);  /* Sender's durable receipt position. */

/* One commit for the group. A newly attempted group returning UNCERTAIN sets
 * first_seq to its candidate identity. Reopen and reconcile before resuming.
 * Calls refused on an already uncertain handle set first_seq to zero. */
mm_journal_status mm_journal_append_group(mm_journal *j, const mm_sample *rows, size_t count,
                                          uint64_t *first_seq);
mm_journal_status mm_journal_append(mm_journal *j, mm_kind kind, uint64_t mono_us, uint32_t boot,
                                    const void *payload, uint32_t len, uint64_t *seq);

/* Caller validates and authorizes the enclosing stream and delivery first.
 * Input is the exact concatenated consecutive record bytes of a batch. */
mm_journal_status mm_journal_ingest(mm_journal *j, const uint8_t *records, size_t len);
/* Returns NOT_FOUND for a missing/reclaimed sequence even if its position
 * is covered by a loss declaration. TOO_SMALL sets *used to the required size. */
mm_journal_status mm_journal_read(mm_journal *j, uint64_t seq, uint8_t *out,
                                  size_t cap, size_t *used);
/* Caller authenticates the issuer. Persist only a matching origin stream and
 * obligation, bounded by the highest locally allocated sequence. */
mm_journal_status mm_journal_confirm(mm_journal *j, const mm_receipt *receipt);
/* Remove fully confirmed origin segments. Receivers retain their records. */
mm_journal_status mm_journal_reclaim(mm_journal *j);

/* Tests may substitute a barrier that calls fsync(fd) or returns an error. */
typedef int (*mm_journal_barrier)(int fd, void *context);
void mm_journal_set_barrier(mm_journal *j, mm_journal_barrier barrier, void *context);

#ifdef __cplusplus
}
#endif
#endif
