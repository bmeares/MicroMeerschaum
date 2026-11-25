// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/actions.h"
#include <string.h>

static const char prompt[] = "mm> ";

static bool write_bytes(mm_action_shell *s, const char *p, size_t len)
{
    if (s->active && len) s->active = s->reply.emit(p, len, s->reply.user);
    return s->active;
}

static bool write_text(mm_action_shell *s, const char *text)
{
    return write_bytes(s, text, strlen(text));
}

bool mm_action_shell_init(mm_action_shell *s, mm_action_view view, mm_action_mode mode,
                          char *reply, size_t cap, mm_action_emit_fn emit, void *ctx, bool echo)
{
    if (!s) return false;
    memset(s, 0, sizeof *s);
    if (!view.registry || !reply || !cap || !emit ||
        (mode != MM_ACTION_QUOTED && mode != MM_ACTION_LITERAL && mode != MM_ACTION_LITERAL_PREFIX)) return false;
    s->view = view;
    s->mode = mode;
    s->echo = echo;
    s->active = true;
    mm_action_output_init(&s->reply, reply, cap);
    s->reply.emit = emit;
    s->reply.user = ctx;
    return write_text(s, prompt);
}

typedef struct {
    mm_action_shell *shell;
    const char *first;
    size_t count;
} candidates;

static void candidate(const char *name, void *ctx)
{
    candidates *c = ctx;
    c->count++;
    if (c->count == 1) {
        c->first = name;
        return;
    }
    if (c->count == 2) {
        write_text(c->shell, "\r\n");
        write_text(c->shell, c->first);
    }
    write_text(c->shell, "\r\n");
    write_text(c->shell, name);
}

static bool complete(mm_action_shell *s)
{
    candidates c = {.shell = s};
    mm_actions_complete(&s->view, s->line, s->len, candidate, &c);
    if (!c.count) return write_text(s, "\a");
    if (c.count == 1) {
        s->len = strlen(c.first);
        memcpy(s->line, c.first, s->len);
        if (s->len < MM_ACTION_MAX_LINE) s->line[s->len++] = ' ';
    }
    write_text(s, "\r\n");
    write_text(s, prompt);
    return write_bytes(s, s->line, s->len);
}

static bool submit(mm_action_shell *s)
{
    if (s->echo && !write_text(s, "\r\n")) return false;
    size_t start = 0, end = s->len;
    while (start < end && s->line[start] == ' ') start++;
    while (end > start && s->line[end - 1] == ' ') end--;
    if (s->discard) {
        write_text(s, "Invalid or overlong line.\r\n");
    } else if (end - start == 4 && memcmp(s->line + start, "exit", 4) == 0) {
        s->active = false;
        return false;
    } else if (end > start) {
        s->line[s->len] = '\0';
        bool ok = mm_actions_dispatch(&s->view, s->line, s->len, s->mode, &s->work, &s->reply);
        if (s->reply.failed) {
            s->active = false;
            return false;
        }
        bool newline = s->reply.len && s->reply.data[s->reply.len - 1] == '\n';
        write_bytes(s, s->reply.data, s->reply.len);
        if (!ok && !s->reply.len) write_text(s, "Action failed.");
        if (!newline) write_text(s, "\r\n");
        if (s->reply.truncated) write_text(s, "Reply truncated.\r\n");
    }
    s->len = 0;
    s->discard = false;
    return write_text(s, prompt);
}

bool mm_action_shell_feed(mm_action_shell *s, unsigned char byte)
{
    if (!s || !s->active) return false;
    if (byte == '\n' && s->after_cr) {
        s->after_cr = false;
        return true;
    }
    s->after_cr = byte == '\r';
    if (byte == '\r' || byte == '\n') return submit(s);
    if (byte == 3) { /* Ctrl-C cancels even an overlong line. */
        s->discard = false;
        s->len = 0;
        write_text(s, "^C\r\n");
        return write_text(s, prompt);
    }
    if (s->discard) return true;
    if (byte == 4 && !s->len) {
        s->active = false;
        return false;
    }
    if (byte == '\t') return complete(s);
    if (byte == 8 || byte == 127) {
        if (s->len) {
            s->len--;
            if (s->echo) return write_text(s, "\b \b");
        }
        return true;
    }
    if (byte < 32 || s->len == MM_ACTION_MAX_LINE) {
        s->discard = true;
        return true;
    }
    s->line[s->len++] = (char)byte;
    return !s->echo || write_bytes(s, s->line + s->len - 1u, 1);
}
