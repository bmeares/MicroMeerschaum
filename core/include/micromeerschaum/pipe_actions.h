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
#ifndef MM_PIPE_DATA_DEFAULT_LIMIT
#define MM_PIPE_DATA_DEFAULT_LIMIT 10u
#endif
#ifndef MM_PIPE_DATA_MAX_LIMIT
#define MM_PIPE_DATA_MAX_LIMIT 128u
#endif
#ifndef MM_VERSION_STRING
#define MM_VERSION_STRING "0.2.1"
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

typedef enum {
    MM_PIPE_SHOW,
    MM_PIPE_SYNC,
    MM_PIPE_COPY,
    MM_PIPE_DATA,
    MM_PIPE_COLUMNS,
    MM_PIPE_ROWCOUNT,
    MM_PIPE_PROGRESS
} mm_pipe_action_kind;
typedef bool (*mm_pipe_admit_fn)(const mm_action_view *, const mm_action_pipe *,
                                 mm_pipe_action_kind, const mm_action_destination *, void *);

typedef struct {
    const mm_action_pipe *pipes;
    size_t pipe_count;
    const mm_action_destination *destinations;
    size_t destination_count;
    /* A NULL predicate permits trusted local views only. SHOW admits discovery,
     * SYNC admits source reads/local writes, COPY admits sending to destination,
     * and DATA/COLUMNS/ROWCOUNT/PROGRESS admit read-only inspection actions. */
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

typedef struct {
    size_t limit;       /* 0..MM_PIPE_DATA_MAX_LIMIT, defaults to MM_PIPE_DATA_DEFAULT_LIMIT */
    const char *begin;  /* optional --begin string, or NULL */
    const char *end;    /* optional --end string, or NULL */
} mm_pipe_data_request;

typedef struct {
    uint64_t prefix;
    uint64_t high;
    uint64_t confirmed;
    uint64_t missed;
} mm_pipe_progress;

typedef bool (*mm_pipe_data_fn)(const mm_action_pipe *, const mm_pipe_data_request *,
                                mm_action_output *, void *);
typedef bool (*mm_pipe_columns_fn)(const mm_action_pipe *, mm_action_output *, void *);
typedef bool (*mm_pipe_rowcount_fn)(const mm_action_pipe *, uint64_t *count, void *);
typedef bool (*mm_pipe_progress_fn)(const mm_action_pipe *, mm_pipe_progress *progress, void *);

typedef struct {
    mm_pipe_data_fn data;          /* optional: installs show data (show datum) */
    mm_pipe_columns_fn columns;    /* optional: installs show columns (show column) */
    mm_pipe_rowcount_fn rowcount;  /* optional: installs show rowcounts (show rowcount) */
    mm_pipe_progress_fn progress;  /* optional: installs show progress */
    const char *version;           /* optional: defaults to "MicroMeerschaum " MM_VERSION_STRING " (native)" */
    void *user;
} mm_pipe_inspect_config;

typedef struct {
    const mm_pipe_action_config *pipes;
    const mm_pipe_inspect_config *inspect;
    mm_action descriptors[6];
    size_t count;
} mm_pipe_inspect_actions;

/* Atomically install opt-in read-only inspection actions. show connectors and
 * show version are always installed; data/columns/rowcounts/progress are
 * installed only when their callbacks in inspect are non-NULL (inspect may be
 * NULL to install connectors and version only). Keep bindings, configs, and
 * referenced objects alive and stable. */
bool mm_pipe_inspect_register(mm_actions *, mm_pipe_inspect_actions *bindings,
                              const mm_pipe_action_config *pipes,
                              const mm_pipe_inspect_config *inspect);

#ifdef __cplusplus
}
#endif
#endif
