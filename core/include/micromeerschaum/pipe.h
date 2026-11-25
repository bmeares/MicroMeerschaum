// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_PIPE_H
#define MICROMEERSCHAUM_PIPE_H

#include "micromeerschaum/document.h"
#include "micromeerschaum/journal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An instance commits the entire group before OK. On UNCERTAIN, return the
 * candidate first sequence if this call attempted a new group. All sample
 * pointers are borrowed only for the duration of append(). */
typedef mm_journal_status (*mm_pipe_append_fn)(void *context, const mm_sample *rows,
                                               size_t count, uint64_t *first_seq);
typedef struct {
    mm_stream stream;
    mm_pipe_append_fn append;
    void *context;
} mm_pipe_target;

typedef struct {
    mm_journal_status status;
    size_t confirmed;         /* Number of input rows durably written by this call. */
    uint64_t uncertain_first; /* Candidate sequence within the target stream, or zero. */
    size_t uncertain_count;
} mm_sync_result;

typedef struct mm_pipe mm_pipe;
/* Supply row index starting at zero per sync call, or set done=true.
 * Return borrowed fields valid until the next fetch. */
typedef mm_journal_status (*mm_pipe_fetch_fn)(void *context, const mm_pipe *pipe,
                                              size_t index, mm_row *row, bool *done);
struct mm_pipe {
    mm_pipe_target target;
    uint8_t *scratch;
    size_t capacity;
    mm_pipe_fetch_fn fetch;
    void *source_context;
};

/* Form a wire key for provisioning. Output uses explicit byte length, no terminator. */
mm_status mm_pipe_key(char *out, size_t cap, size_t *used, const char *connector,
                      const char *metric, const char *location);

/* Bind a local handle to a provisioned target. The target's key bytes, context
 * and caller-owned scratch must remain valid for the lifetime of the handle.
 * Opening checks the identity locally. Serialize operations on a handle. */
mm_journal_status mm_pipe_open(mm_pipe *out, const char *connector, const char *metric,
                               const char *location, mm_pipe_target target,
                               uint8_t *scratch, size_t capacity);
mm_sync_result mm_pipe_sync_rows(mm_pipe *pipe, const mm_row *rows, size_t count,
                                 uint64_t mono_us, uint32_t boot);
mm_sync_result mm_pipe_sync_binary(mm_pipe *pipe, const void *payload, size_t len,
                                   uint64_t mono_us, uint32_t boot);
/* name is the plugin name without its 'plugin:' prefix. */
mm_journal_status mm_pipe_bind_source(mm_pipe *pipe, const char *name, mm_pipe_fetch_fn fetch,
                                      void *context);
mm_sync_result mm_pipe_sync_source(mm_pipe *pipe, uint64_t mono_us, uint32_t boot);

#ifdef __cplusplus
}
#endif
#endif
