// SPDX-License-Identifier: Apache-2.0
#ifndef MM_STORAGE_VOLUME_H
#define MM_STORAGE_VOLUME_H
#include "micromeerschaum/journal.h"

#define MM_STATE_MAX (36u + MM_STREAM_MAX)
#define MM_ROUTE_BYTES 2040u
#define MM_VOLUME_MAX_SLOTS 64u
typedef struct { uint64_t first, last; } seq_range;
typedef struct { seq_range range; size_t size; } segment;
typedef struct mm_volume mm_volume;
mm_journal_status mm_volume_open(mm_volume **out, const char *path, const mm_volume_options *opt,
                                 bool create);
mm_journal_status mm_volume_repair(mm_volume *v);
void mm_volume_close(mm_volume *v);
size_t mm_volume_limit(const mm_volume *v);
size_t mm_volume_count(const mm_volume *v);
size_t mm_volume_capacity(const mm_volume *v);
mm_retention mm_volume_retention(const mm_volume *v);
uint64_t mm_volume_missed(const mm_volume *v);
segment mm_volume_segment(const mm_volume *v, size_t ordinal);
const uint8_t *mm_volume_routes(mm_volume *v, size_t *len);
void mm_volume_stage_routes(mm_volume *v, const uint8_t *data, size_t len);
mm_journal_status mm_volume_commit_routes(mm_volume *v);
mm_journal_status mm_volume_state(mm_volume *v, uint8_t **out, size_t *size);
mm_journal_status mm_volume_save_state(mm_volume *v, const uint8_t *state, size_t size);
mm_journal_status mm_volume_read(mm_volume *v, seq_range range, uint8_t **out, size_t *size);
mm_journal_status mm_volume_publish(mm_volume *v, seq_range range, const uint8_t *header, size_t h,
                                    const uint8_t *records, size_t n, uint64_t remove_through,
                                    uint64_t missed);
mm_journal_status mm_volume_trim(mm_volume *v, uint64_t through);
mm_journal_status mm_volume_set_missed(mm_volume *v, uint64_t missed);
void mm_volume_set_barrier(mm_volume *v, mm_journal_barrier fn, void *context);
#endif
