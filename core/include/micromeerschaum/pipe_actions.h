// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_PIPE_ACTIONS_H
#define MICROMEERSCHAUM_PIPE_ACTIONS_H

#include "micromeerschaum/actions.h"
#include "micromeerschaum/pipe.h"
#include "micromeerschaum/sender.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MM_PIPE_ACTION_MAX_VALUES
#define MM_PIPE_ACTION_MAX_VALUES 8u
#endif

/* An explicitly published, already-open handle. The action reads connector,
 * metric and location from the handle's provisioned stream key. */
typedef struct {
    mm_pipe *pipe;
    const char *instance;
    void *user;
} mm_action_pipe;

typedef struct {
    const char *name;
    /* Deliver retained records through the configured destination. Report
     * confirmed progress even if a subsequent exchange fails. */
    mm_journal_status (*deliver)(const mm_action_pipe *, mm_send_result *, void *);
    void *user;
} mm_action_destination;

typedef enum { MM_PIPE_SHOW, MM_PIPE_SYNC, MM_PIPE_COPY } mm_pipe_action_kind;
typedef bool (*mm_pipe_admit_fn)(const mm_action_view *, const mm_action_pipe *,
                                 mm_pipe_action_kind, const mm_action_destination *, void *);

typedef struct {
    const mm_action_pipe *pipes;
    size_t pipe_count;
    const mm_action_destination *destinations;
    size_t destination_count;
    /* A NULL predicate permits trusted local views only. SHOW admits discovery,
     * SYNC admits source reads/local writes, COPY admits sending to destination. */
    mm_pipe_admit_fn admit;
    uint64_t (*mono_us)(void *); /* optional sample timestamp, defaults to zero */
    uint32_t boot;
    void *user;
} mm_pipe_action_config;

typedef struct {
    const mm_pipe_action_config *config;
    mm_action descriptors[3];
} mm_pipe_actions;

/* Atomically install show pipes, sync pipes and copy pipes. Keep bindings,
 * config, both tables and their referenced objects alive and stable. Serialize
 * access to each Pipe while actions are running. Use fresh binding storage
 * for each registry. An empty inventory is supported. */
bool mm_pipe_actions_register(mm_actions *, mm_pipe_actions *bindings,
                              const mm_pipe_action_config *);

#ifdef __cplusplus
}
#endif
#endif
