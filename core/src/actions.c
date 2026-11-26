// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/actions.h"
#include <string.h>

static bool space(char c) { return c == ' ' || c == '\t'; }

static bool valid_line(const char *line, size_t len)
{
    if (!line || len > MM_ACTION_MAX_LINE) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)line[i];
        if ((c < 32 && c != '\t') || c == 127) return false;
    }
    return true;
}

static bool valid_name(const char *name)
{
    if (!name || !name[0] || name[0] == ' ') return false;
    for (size_t i = 0; i <= MM_ACTION_MAX_LINE; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!c) return true;
        if (c < 32 || c > 126 || c == '\'' || c == '"' || c == '\\') return false;
        if (c == ' ' && (!name[i + 1] || name[i + 1] == ' ')) return false;
    }
    return false;
}

static bool named(const mm_action *a, const char *name)
{
    return strcmp(name, a->name) == 0 || (a->alias && strcmp(name, a->alias) == 0);
}

bool mm_actions_register(mm_actions *r, const mm_action *a)
{
    if (!r || !r->slots || r->count >= r->capacity || !a || !a->run || !valid_name(a->name))
        return false;
    if (a->alias && (!valid_name(a->alias) || strcmp(a->name, a->alias) == 0)) return false;
    for (size_t i = 0; i < r->count; i++) {
        if (named(r->slots[i], a->name) || (a->alias && named(r->slots[i], a->alias)))
            return false;
    }
    r->slots[r->count++] = a;
    return true;
}

static bool help(const mm_action_call *, mm_action_output *, void *);
static bool show_actions(const mm_action_call *, mm_action_output *, void *);

bool mm_actions_init_with_help(mm_actions *r, const mm_action **slots, size_t capacity,
                               const mm_action *help_action)
{
    static const mm_action list_action = {"show actions", "show action", "List available actions.",
                                         show_actions, NULL};
    if (!r) return false;
    *r = (mm_actions){0};
    if (!slots || capacity < 2 || !help_action || !help_action->name ||
        strcmp(help_action->name, "help") != 0) return false;
    *r = (mm_actions){slots, 0, capacity};
    if (!mm_actions_register(r, help_action) || !mm_actions_register(r, &list_action)) {
        while (r->count) r->slots[--r->count] = NULL;
        *r = (mm_actions){0};
        return false;
    }
    return true;
}

bool mm_actions_init(mm_actions *r, const mm_action **slots, size_t capacity)
{
    static const mm_action help_action = {"help", NULL, "help [ACTION]", help, NULL};
    return mm_actions_init_with_help(r, slots, capacity, &help_action);
}

static bool allow_local(const mm_action *a, void *ctx)
{
    (void)a;
    (void)ctx;
    return true;
}

mm_action_view mm_actions_local(const mm_actions *r)
{
    return (mm_action_view){r, allow_local, NULL, true};
}

mm_action_view mm_actions_remote(const mm_actions *r, mm_action_admit_fn admit, void *ctx)
{
    return (mm_action_view){r, admit, ctx, false};
}

static bool visible(const mm_action_view *view, const mm_action *a)
{
    return view->admit && view->admit(a, view->user);
}

size_t mm_actions_visit(const mm_action_view *view, mm_action_visit_fn visit, void *ctx)
{
    if (!view || !view->registry) return 0;
    size_t count = 0;
    for (size_t i = 0; i < view->registry->count; i++) {
        const mm_action *a = view->registry->slots[i];
        if (visible(view, a)) {
            count++;
            if (visit) visit(a, ctx);
        }
    }
    return count;
}

void mm_action_output_init(mm_action_output *out, char *data, size_t cap)
{
    if (!out) return;
    *out = (mm_action_output){.data = data, .capacity = data ? cap : 0};
    if (out->capacity) data[0] = '\0';
}

bool mm_action_write(mm_action_output *out, const char *data, size_t len)
{
    if (!out || (!data && len) || out->failed || out->truncated) return false;
    size_t room = out->capacity ? out->capacity - out->len - 1u : 0;
    size_t n = len < room ? len : room;
    if (n) {
        memcpy(out->data + out->len, data, n);
        out->len += n;
        out->data[out->len] = '\0';
    }
    out->truncated = n < len;
    return !out->truncated;
}

bool mm_action_puts(mm_action_output *out, const char *s)
{
    return s && mm_action_write(out, s, strlen(s));
}

bool mm_action_flush(mm_action_output *out)
{
    if (!out || !out->emit || out->failed) return false;
    if (out->len && !out->emit(out->data, out->len, out->user)) {
        out->failed = true;
        return false;
    }
    out->len = 0;
    if (out->capacity) out->data[0] = '\0';
    return !out->truncated;
}

static bool error(mm_action_output *out, const char *text)
{
    mm_action_puts(out, text);
    return false;
}

/* Match unquoted action words before touching the argument tail. Spaces and
 * tabs in input match one canonical space. A partial last word never wins. */
static size_t matched(const char *name, const char *line, size_t len)
{
    if (!name) return 0;
    size_t p = 0;
    for (; *name; name++) {
        if (p == len) return 0;
        if (*name == ' ') {
            if (!space(line[p])) return 0;
            do { p++; } while (p < len && space(line[p]));
        } else {
            if (line[p++] != *name) return 0;
        }
    }
    return p == len || space(line[p]) ? p : 0;
}

/* Each copied byte consumes an input byte. Each NUL except the last can
 * occupy space freed by an argument separator. */
static const char *arguments(mm_action_call *call, mm_action_mode mode, char *buf,
                             const char **argv)
{
    size_t p = 0;
    const char *s = call->raw_tail;
    while (p < call->tail_len) {
        if (call->argc == MM_ACTION_MAX_ARGS) {
            if (mode == MM_ACTION_LITERAL_PREFIX) break;
            return "Too many arguments.";
        }
        argv[call->argc++] = buf;
        char quote = 0;
        while (p < call->tail_len && (quote || !space(s[p]))) {
            char c = s[p++];
            if (mode == MM_ACTION_QUOTED) {
                if (c == '\\' && quote != '\'') {
                    if (p == call->tail_len) return "Incomplete escape.";
                    c = s[p++];
                } else if (quote && c == quote) {
                    quote = 0;
                    continue;
                } else if (!quote && (c == '\'' || c == '"')) {
                    quote = c;
                    continue;
                }
            }
            *buf++ = c;
        }
        if (quote) return "Unterminated quote.";
        *buf++ = '\0';
        while (p < call->tail_len && space(s[p])) p++;
    }
    argv[call->argc] = NULL;
    call->argv = argv;
    return NULL;
}

bool mm_actions_dispatch(const mm_action_view *view, const char *line, size_t len,
                         mm_action_mode mode, mm_action_workspace *work, mm_action_output *out)
{
    if (!out) return false;
    out->len = 0;
    out->truncated = out->failed = false;
    if (out->capacity) out->data[0] = '\0';
    if (!view || !view->registry || !work || !valid_line(line, len) ||
        (mode != MM_ACTION_QUOTED && mode != MM_ACTION_LITERAL && mode != MM_ACTION_LITERAL_PREFIX))
        return error(out, "Invalid action line.");
    size_t start = 0;
    while (start < len && space(line[start])) start++;
    size_t best = 0;
    const mm_action *action = NULL;
    for (size_t i = 0; i < view->registry->count; i++) {
        const mm_action *a = view->registry->slots[i];
        size_t n = matched(a->name, line + start, len - start);
        size_t alias = matched(a->alias, line + start, len - start);
        if (alias > n) n = alias;
        if (n > best) {
            best = n;
            action = a;
        }
    }
    /* A denied specific action may never fall through to a shorter match. */
    if (!action || !visible(view, action)) return error(out, "Unknown action.");
    size_t tail = start + best;
    while (tail < len && space(line[tail])) tail++;
    mm_action_call call = {.view = view, .raw_line = line, .raw_len = len,
                           .raw_tail = line + tail, .tail_len = len - tail};
    const char *argv[MM_ACTION_MAX_ARGS + 1u];
    const char *problem = arguments(&call, mode, work->decoded, argv);
    if (problem) return error(out, problem);
    return action->run(&call, out, action->user);
}

static void describe(const mm_action *a, void *ctx)
{
    mm_action_output *out = ctx;
    mm_action_puts(out, a->name);
    if (a->alias) {
        mm_action_puts(out, " (");
        mm_action_puts(out, a->alias);
        mm_action_puts(out, ")");
    }
    if (a->help && a->help[0]) {
        mm_action_puts(out, "  ");
        mm_action_puts(out, a->help);
    }
    mm_action_puts(out, "\n");
}

static bool show_actions(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    (void)ctx;
    if (call->argc) return error(out, "Usage: show actions");
    mm_actions_visit(call->view, describe, out);
    return true;
}

static bool help(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    (void)ctx;
    return mm_actions_help(call, out);
}

bool mm_actions_help(const mm_action_call *call, mm_action_output *out)
{
    if (!call || !call->view || !call->view->registry || !out) return false;
    if (!call->argc) return show_actions(call, out, NULL);
    size_t len = call->tail_len;
    while (len && space(call->raw_tail[len - 1])) len--;
    for (size_t i = 0; i < call->view->registry->count; i++) {
        const mm_action *a = call->view->registry->slots[i];
        if ((matched(a->name, call->raw_tail, len) == len ||
             matched(a->alias, call->raw_tail, len) == len) && visible(call->view, a)) {
            describe(a, out);
            return true;
        }
    }
    return error(out, "Unknown action.");
}

static bool starts_with(const char *name, const char *line, size_t len)
{
    if (!name) return false;
    size_t p = 0;
    while (p < len && space(line[p])) p++;
    while (p < len) {
        if (space(line[p])) {
            if (*name++ != ' ') return false;
            do { p++; } while (p < len && space(line[p]));
        } else if (line[p++] != *name++) {
            return false;
        }
    }
    return true;
}

size_t mm_actions_complete(const mm_action_view *view, const char *prefix, size_t len,
                           mm_action_candidate_fn visit, void *ctx)
{
    if (!view || !view->registry || !valid_line(prefix, len)) return 0;
    size_t count = 0;
    for (size_t i = 0; i < view->registry->count; i++) {
        const mm_action *a = view->registry->slots[i];
        if (!visible(view, a)) continue;
        const char *names[] = {a->name, a->alias};
        for (size_t j = 0; j < 2; j++) {
            if (starts_with(names[j], prefix, len)) {
                count++;
                if (visit) visit(names[j], ctx);
            }
        }
    }
    return count;
}
