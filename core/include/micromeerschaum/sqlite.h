// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_SQLITE_H
#define MICROMEERSCHAUM_SQLITE_H

#include "micromeerschaum/pipe.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MM_SQLITE_MAX_COLUMNS 16u
#define MM_SQLITE_MAX_NAMESIZE 63u
#define MM_SQLITE_MAX_VALUES 64u
#define MM_SQLITE_MAX_LIMIT 1024u

typedef struct mm_sqlite mm_sqlite;
typedef struct mm_sqlite_cursor mm_sqlite_cursor;
typedef struct {
    const char *name; /* NUL-terminated UTF-8. */
    mm_value_type type;
    bool key; /* Additional key column. The axis is always a key. */
} mm_sqlite_column;
typedef struct {
    const mm_sqlite_column *columns;
    size_t count;
    size_t axis; /* Index of an INT or TIME column. */
    bool autotime; /* Generate a missing TIME axis using the configured UTC clock. */
} mm_sqlite_definition;

/* One database file per provisioned Pipe. NULL definition reopens an existing
 * file; a supplied definition creates or verifies the registration. The origin
 * configuration supplies the stream and immutable delivery policy. max_bytes=0.
 * Serialize operations per handle. Finalize cursors before writes or close. */
mm_journal_status mm_sqlite_open(mm_sqlite **out, const char *path,
                                 const mm_journal_config *config,
                                 const mm_sqlite_definition *definition);
void mm_sqlite_close(mm_sqlite *db);
const mm_sqlite_definition *mm_sqlite_definition_get(const mm_sqlite *db);
mm_pipe_target mm_sqlite_target(mm_sqlite *db);
typedef bool (*mm_sqlite_clock)(void *context, int64_t *utc_us);
void mm_sqlite_set_clock(mm_sqlite *db, mm_sqlite_clock clock, void *context);

typedef struct {
    const char *column;
    const mm_value *values; /* Equality within a list. Empty list matches no rows. */
    size_t count;
} mm_sqlite_filter;
typedef struct {
    const mm_value *begin, *end; /* Optional half-open bounds in axis units. */
    const mm_sqlite_filter *filters; /* AND across named columns. */
    size_t filter_count;
    size_t limit; /* Explicit 0..MM_SQLITE_MAX_LIMIT for get_docs. */
} mm_sqlite_query;

/* Each row borrows the cursor's storage until the next step or close.
 * NOT_FOUND signals successful exhaustion. Close even on iteration errors. */
mm_journal_status mm_sqlite_get_docs(mm_sqlite *db, const mm_sqlite_query *query,
                                     mm_sqlite_cursor **out);
mm_journal_status mm_sqlite_next(mm_sqlite_cursor *cursor, mm_row *row);
void mm_sqlite_cursor_close(mm_sqlite_cursor *cursor);
/* Returns a NULL value for an empty selection. Ignores query.limit. */
mm_journal_status mm_sqlite_get_sync_time(mm_sqlite *db, const mm_sqlite_query *query,
                                          mm_value *result);

mm_journal_status mm_sqlite_positions(mm_sqlite *db, uint64_t *high, uint64_t *confirmed);
mm_journal_status mm_sqlite_read(mm_sqlite *db, uint64_t seq, uint8_t *out, size_t cap,
                                 size_t *used);
/* Builds the next pending batch. NOT_FOUND means the outbox is caught up.
 * TOO_SMALL sets used to the required size for the first pending record. */
mm_journal_status mm_sqlite_batch(mm_sqlite *db, uint8_t *out, size_t cap, size_t *used);
/* Atomically persist the matching receipt and reclaim that outbox prefix.
 * Caller supplies the peer identity authenticated by the transport. */
mm_journal_status mm_sqlite_confirm(mm_sqlite *db, uint64_t authenticated_issuer,
                                    const mm_receipt *receipt);

#ifdef __cplusplus
}
#endif
#endif
