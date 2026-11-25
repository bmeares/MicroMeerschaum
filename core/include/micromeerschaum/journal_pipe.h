// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_JOURNAL_PIPE_H
#define MICROMEERSCHAUM_JOURNAL_PIPE_H

#include "micromeerschaum/pipe.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline mm_journal_status mm_pipe_journal_append(void *context, const mm_sample *rows,
                                                       size_t count, uint64_t *first_seq)
{
    return mm_journal_append_group((mm_journal *)context, rows, count, first_seq);
}

static inline mm_pipe_target mm_pipe_journal_target(mm_journal *journal)
{
    mm_pipe_target target = {{0, 0, NULL, 0}, NULL, NULL};
    if (journal) {
        const mm_journal_config *config = mm_journal_config_get(journal);
        if (config->mode == MM_JOURNAL_ORIGIN) {
            target.stream = config->stream;
            target.append = mm_pipe_journal_append;
            target.context = journal;
        }
    }
    return target;
}

#ifdef __cplusplus
}
#endif
#endif
