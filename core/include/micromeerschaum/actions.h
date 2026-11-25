// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_ACTIONS_H
#define MICROMEERSCHAUM_ACTIONS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Use the same limits in all translation units. The line length excludes its terminator. */
#ifndef MM_ACTION_MAX_LINE
#define MM_ACTION_MAX_LINE 4224u
#endif
#ifndef MM_ACTION_MAX_ARGS
#define MM_ACTION_MAX_ARGS 32u
#endif

typedef struct mm_action_view mm_action_view;
typedef struct mm_action_output mm_action_output;
typedef struct {
    const mm_action_view *view;
    const char *raw_line, *raw_tail; /* original byte spans, no added NUL */
    size_t raw_len, tail_len;
    size_t argc;
    const char *const *argv;        /* decoded, NUL-terminated arguments; argv[argc] == NULL */
} mm_action_call;

typedef bool (*mm_action_fn)(const mm_action_call *, mm_action_output *, void *);
typedef struct {
    const char *name, *alias, *help; /* alias and help may be NULL */
    mm_action_fn run;
    void *user;
} mm_action;
typedef struct {
    const mm_action **slots;
    size_t count, capacity;
} mm_actions;

/* Two slots are used for help and show actions. Registry slots and registered
 * immutable descriptors (including their strings) must outlive all uses.
 * Finish registration before concurrently reading the registry. */
bool mm_actions_init(mm_actions *, const mm_action **slots, size_t capacity);
/* Install the application's canonical "help" descriptor, including its alias
 * and legacy reply. The standard "show actions" descriptor is also installed. */
bool mm_actions_init_with_help(mm_actions *, const mm_action **slots, size_t capacity,
                               const mm_action *help);
bool mm_actions_register(mm_actions *, const mm_action *);

/* This predicate sees the canonical descriptor even when an alias is used. */
typedef bool (*mm_action_admit_fn)(const mm_action *, void *);
struct mm_action_view {
    const mm_actions *registry;
    mm_action_admit_fn admit;
    void *user;
    bool local;
};
mm_action_view mm_actions_local(const mm_actions *);
mm_action_view mm_actions_remote(const mm_actions *, mm_action_admit_fn, void *user);

/* The callback confirms synchronous acceptance of the complete span. */
typedef bool (*mm_action_emit_fn)(const char *, size_t, void *);
struct mm_action_output {
    char *data;
    size_t capacity, len;
    bool truncated, failed;
    mm_action_emit_fn emit;    /* optional flush destination */
    void *user;
};
void mm_action_output_init(mm_action_output *, char *data, size_t capacity);
bool mm_action_write(mm_action_output *, const char *data, size_t len);
bool mm_action_puts(mm_action_output *, const char *);
bool mm_action_flush(mm_action_output *);

typedef enum {
    MM_ACTION_QUOTED,          /* single/double quotes and backslash escapes */
    MM_ACTION_LITERAL,         /* split on space/tab, preserve every other byte */
    MM_ACTION_LITERAL_PREFIX   /* same, expose at most MAX_ARGS; raw tail is complete */
} mm_action_mode;

/* One scratch buffer per overlapping dispatch. A static workspace suits a
 * single-task device. Nested dispatch needs a second workspace. */
typedef struct { char decoded[MM_ACTION_MAX_LINE + 1u]; } mm_action_workspace;

/* Each call clears previous output and invokes at most one handler. The return
 * value is the action outcome. Check truncated/failed separately before using
 * the reply. The raw spans and argv are valid only during the callback. */
bool mm_actions_dispatch(const mm_action_view *, const char *line, size_t len,
                         mm_action_mode, mm_action_workspace *, mm_action_output *);

/* The standard, view-filtered help formatter. A custom help handler may call
 * it for a restricted view while preserving a local console's legacy reply. */
bool mm_actions_help(const mm_action_call *, mm_action_output *);

/* Visits each admitted descriptor once, in registration order. Completion
 * visits each matching full name or alias. The empty prefix matches all. */
typedef void (*mm_action_visit_fn)(const mm_action *, void *);
typedef void (*mm_action_candidate_fn)(const char *name, void *);
size_t mm_actions_visit(const mm_action_view *, mm_action_visit_fn, void *user);
size_t mm_actions_complete(const mm_action_view *, const char *prefix, size_t len,
                           mm_action_candidate_fn, void *user);

typedef struct {
    mm_action_view view;
    mm_action_workspace work;
    mm_action_output reply;
    mm_action_mode mode;
    size_t len;
    bool echo, active, discard, after_cr;
    char line[MM_ACTION_MAX_LINE + 1u];
} mm_action_shell;

/* Emits the initial prompt. Feed one byte at a time. false means the session
 * ended (exit, Ctrl-D on an empty line, or output failure). The caller owns I/O.
 * Turn echo off if the terminal driver already performs character echo. */
bool mm_action_shell_init(mm_action_shell *, mm_action_view, mm_action_mode,
                          char *reply, size_t capacity, mm_action_emit_fn, void *user, bool echo);
bool mm_action_shell_feed(mm_action_shell *, unsigned char byte);

#ifdef __cplusplus
}
#endif
#endif
