// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_DEVICE_SESSION_H
#define MICROMEERSCHAUM_DEVICE_SESSION_H

#include "micromeerschaum/actions.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MM_SESSION_FRAME_MAX 4096u
#define MM_SESSION_PREFIX "@micro "
#define MM_SESSION_SERVICE_UUID "2f725f70-9771-4f95-bb68-5e47ea680001"
#define MM_SESSION_RX_UUID      "2f725f70-9771-4f95-bb68-5e47ea680002"
#define MM_SESSION_TX_UUID      "2f725f70-9771-4f95-bb68-5e47ea680003"

struct cJSON;
enum { MM_SESSION_FILTER_BEGIN = 1, MM_SESSION_FILTER_END = 2, MM_SESSION_FILTER_PARAMS = 4 };
typedef struct {
    int id;
    const char *connector, *metric, *location, *instance;
    /* request is borrowed and includes limit and any supplied selectors.
     * Append at most limit documents to rows. Reject unsupported selection. */
    bool (*read)(const struct cJSON *request, struct cJSON *rows, mm_action_output *, void *);
    /* rows is borrowed. Return success only after the local write completes. */
    bool (*write)(const struct cJSON *rows, mm_action_output *, void *);
    void *user;
    unsigned filters; /* supported read selections; zero for an unfiltered journal */
} mm_session_pipe;

typedef enum { MM_SESSION_DISCOVER, MM_SESSION_READ, MM_SESSION_WRITE } mm_session_access;
typedef struct {
    mm_action_view view;       /* construct with mm_actions_remote */
    mm_action_mode mode;
    const mm_session_pipe *pipes;
    size_t pipe_count;
    bool (*allow_op)(const char *op, void *);
    bool (*allow_pipe)(const mm_session_pipe *, mm_session_access, void *);
    void *user;
} mm_device_session_config;

typedef struct {
    const mm_device_session_config *config;
    mm_action_workspace work;
    char line[MM_SESSION_FRAME_MAX + 1u], reply[MM_SESSION_FRAME_MAX + 8u];
    size_t used;
    bool discard;
} mm_device_session;

/* Allocate separately for each connection and serialize access. Keep config
 * and its registration table alive and stable. An absent predicate denies. */
bool mm_device_session_init(mm_device_session *, const mm_device_session_config *);
void mm_device_session_reset(mm_device_session *);
/* Includes prefix and terminating LF. Invalid, uncorrelatable and ordinary
 * console lines are ignored. false indicates invalid API usage or lost output.
 * On lost output the caller ends the connection without replay. */
bool mm_device_session_line(mm_device_session *, const char *, size_t, mm_action_emit_fn, void *io);
bool mm_device_session_feed(mm_device_session *, const char *, size_t, mm_action_emit_fn, void *io);

#ifdef __cplusplus
}
#endif
#endif
