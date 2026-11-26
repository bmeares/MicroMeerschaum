// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/actions.h"
#include <string.h>

static const char default_instance_name[] = "journal:local";
static const char unicode_arrow[] = "\xe2\x9e\xa4"; /* U+27A4 */

static bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

static bool write_bytes(mm_action_shell *s, const char *p, size_t len)
{
    if (s->active && len) s->active = s->reply.emit(p, len, s->reply.user);
    return s->active;
}

static bool write_text(mm_action_shell *s, const char *text)
{
    return write_bytes(s, text, strlen(text));
}

static bool write_status_prefix(mm_action_shell *s, bool ok)
{
    if (s->ansi) return write_text(s, ok ? "\x1b[32m+ \x1b[0m" : "\x1b[31m- \x1b[0m");
    return write_text(s, ok ? "+ " : "- ");
}

static bool write_status_line(mm_action_shell *s, bool ok, const char *text)
{
    write_status_prefix(s, ok);
    write_text(s, text);
    return write_text(s, "\r\n");
}

static bool write_prompt(mm_action_shell *s)
{
    const char *arrow = s->unicode ? unicode_arrow : ">";
    if (s->ansi) {
        write_text(s, "\x1b[32m[\x1b[0m \x1b[36m");
        write_text(s, s->instance);
        write_text(s, "\x1b[0m \x1b[32m] ");
        write_text(s, arrow);
        return write_text(s, "\x1b[0m ");
    }
    write_text(s, "[ ");
    write_text(s, s->instance);
    write_text(s, " ] ");
    write_text(s, arrow);
    return write_text(s, " ");
}

static bool valid_instance(const char *p, size_t len)
{
    if (!p || !len || len > MM_ACTION_SHELL_INSTANCE_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c <= 32 || c > 126 || c == '\'' || c == '"' || c == '\\') return false;
    }
    return true;
}

bool mm_action_shell_set_instance(mm_action_shell *s, const char *instance)
{
    if (!s) return false;
    if (!instance) {
        memcpy(s->instance, s->default_instance, strlen(s->default_instance) + 1u);
        s->filter_instance = false;
        return true;
    }
    size_t n = strlen(instance);
    if (!valid_instance(instance, n)) return false;
    memcpy(s->instance, instance, n);
    s->instance[n] = '\0';
    s->filter_instance = true;
    return true;
}

bool mm_action_shell_init_ex(mm_action_shell *s, mm_action_view view, mm_action_mode mode,
                             char *reply, size_t cap, mm_action_emit_fn emit, void *ctx,
                             bool echo, const mm_action_shell_options *options)
{
    if (!s) return false;
    memset(s, 0, sizeof *s);
    if (!view.registry || !reply || !cap || !emit ||
        (mode != MM_ACTION_QUOTED && mode != MM_ACTION_LITERAL && mode != MM_ACTION_LITERAL_PREFIX))
        return false;
    const char *inst = (options && options->instance) ? options->instance : default_instance_name;
    size_t inst_len = strlen(inst);
    if (!valid_instance(inst, inst_len)) return false;
    const char *ver = (options && options->version && options->version[0]) ? options->version : MM_VERSION;

    s->view = view;
    s->mode = mode;
    s->echo = echo;
    s->active = true;
    s->ansi = options && options->ansi;
    s->unicode = options && options->unicode;
    memcpy(s->default_instance, inst, inst_len + 1u);
    memcpy(s->instance, inst, inst_len + 1u);
    mm_action_output_init(&s->reply, reply, cap);
    s->reply.emit = emit;
    s->reply.user = ctx;

    write_text(s, "MicroMeerschaum ");
    write_text(s, ver);
    write_text(s, "\r\n");
    return write_prompt(s);
}

bool mm_action_shell_init(mm_action_shell *s, mm_action_view view, mm_action_mode mode,
                          char *reply, size_t cap, mm_action_emit_fn emit, void *ctx, bool echo)
{
    return mm_action_shell_init_ex(s, view, mode, reply, cap, emit, ctx, echo, NULL);
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
        s->hist_pos = 0;
    }
    write_text(s, "\r\n");
    write_prompt(s);
    return write_bytes(s, s->line, s->len);
}

static void list_candidate(const char *name, void *ctx)
{
    candidates *c = ctx;
    c->count++;
    write_status_line(c->shell, true, name);
}

static void history_push(mm_action_shell *s, const char *text, size_t len)
{
    if (!len || !MM_ACTION_SHELL_HISTORY) return;
    size_t copy = len < MM_ACTION_SHELL_HISTORY_LINE ? len : MM_ACTION_SHELL_HISTORY_LINE;
    if (s->hist_count) {
        size_t last = (s->hist_head + MM_ACTION_SHELL_HISTORY - 1u) % MM_ACTION_SHELL_HISTORY;
        if (strlen(s->history[last]) == copy && memcmp(s->history[last], text, copy) == 0) return;
    }
    memcpy(s->history[s->hist_head], text, copy);
    s->history[s->hist_head][copy] = '\0';
    s->hist_head = (s->hist_head + 1u) % MM_ACTION_SHELL_HISTORY;
    if (s->hist_count < MM_ACTION_SHELL_HISTORY) s->hist_count++;
}

static bool replace_line(mm_action_shell *s, const char *text, size_t len)
{
    if (s->echo) {
        for (size_t i = 0; i < s->len; i++) {
            if (!write_text(s, "\b \b")) return false;
        }
    }
    size_t copy = len < MM_ACTION_MAX_LINE ? len : MM_ACTION_MAX_LINE;
    if (copy) memcpy(s->line, text, copy);
    s->len = copy;
    return !s->echo || write_bytes(s, s->line, s->len);
}

static bool history_move(mm_action_shell *s, int delta)
{
    if (!s->hist_count) return true;
    if (delta > 0) {
        if (s->hist_pos >= s->hist_count) return true;
        s->hist_pos++;
    } else {
        if (s->hist_pos == 0) return true;
        s->hist_pos--;
        if (s->hist_pos == 0) return replace_line(s, "", 0);
    }
    size_t idx = (s->hist_head + MM_ACTION_SHELL_HISTORY - s->hist_pos) % MM_ACTION_SHELL_HISTORY;
    const char *entry = s->history[idx];
    return replace_line(s, entry, strlen(entry));
}

static bool word_eq(const char *p, size_t len, const char *lit)
{
    return strlen(lit) == len && memcmp(p, lit, len) == 0;
}

static bool is_pipe_action_line(const char *line, size_t len, size_t *tail_out)
{
    size_t i = 0;
    while (i < len && is_space(line[i])) i++;
    size_t w1_start = i;
    while (i < len && !is_space(line[i])) i++;
    size_t w1_len = i - w1_start;
    while (i < len && is_space(line[i])) i++;
    size_t w2_start = i;
    while (i < len && !is_space(line[i])) i++;
    size_t w2_len = i - w2_start;
    if (!w1_len || !w2_len) return false;
    const char *w1 = line + w1_start;
    const char *w2 = line + w2_start;
    bool match = false;
    if (word_eq(w1, w1_len, "show")) {
        match = word_eq(w2, w2_len, "pipes") || word_eq(w2, w2_len, "pipe") ||
                word_eq(w2, w2_len, "data") || word_eq(w2, w2_len, "datum") ||
                word_eq(w2, w2_len, "columns") || word_eq(w2, w2_len, "column") ||
                word_eq(w2, w2_len, "rowcounts") || word_eq(w2, w2_len, "rowcount") ||
                word_eq(w2, w2_len, "progress");
    } else if (word_eq(w1, w1_len, "sync") || word_eq(w1, w1_len, "copy")) {
        match = word_eq(w2, w2_len, "pipes") || word_eq(w2, w2_len, "pipe");
    }
    if (match && tail_out) *tail_out = i;
    return match;
}

static bool tail_has_instance_flag(const char *s, size_t len, mm_action_mode mode)
{
    size_t p = 0;
    while (p < len) {
        while (p < len && is_space(s[p])) p++;
        if (p == len) break;
        char tok[20];
        size_t tlen = 0;
        bool overflow = false;
        char quote = 0;
        while (p < len && (quote || !is_space(s[p]))) {
            char c = s[p++];
            if (mode == MM_ACTION_QUOTED) {
                if (c == '\\' && quote != '\'') {
                    if (p == len) return true; /* malformed: skip injection */
                    c = s[p++];
                } else if (quote && c == quote) {
                    quote = 0;
                    continue;
                } else if (!quote && (c == '\'' || c == '"')) {
                    quote = c;
                    continue;
                }
            }
            if (tlen < sizeof tok) tok[tlen++] = c;
            else overflow = true;
        }
        if (quote) return true; /* malformed: skip injection */
        if (!overflow && (word_eq(tok, tlen, "-i") ||
                          word_eq(tok, tlen, "--instance") ||
                          word_eq(tok, tlen, "--mrsm-instance"))) {
            return true;
        }
    }
    return false;
}

static bool handle_instance_cmd(mm_action_shell *s, const char *args, size_t len)
{
    size_t i = 0;
    while (i < len && is_space(args[i])) i++;
    if (i == len) return write_status_line(s, true, s->instance);
    size_t arg_start = i;
    while (i < len && !is_space(args[i])) i++;
    size_t arg_len = i - arg_start;
    while (i < len && is_space(args[i])) i++;
    if (i < len) return write_status_line(s, false, "Usage: instance [KEYS|all|default|reset]");

    const char *arg = args + arg_start;
    if (word_eq(arg, arg_len, "all") || word_eq(arg, arg_len, "default") ||
        word_eq(arg, arg_len, "reset")) {
        mm_action_shell_set_instance(s, NULL);
    } else {
        if (!valid_instance(arg, arg_len)) return write_status_line(s, false, "Invalid instance keys.");
        memcpy(s->instance, arg, arg_len);
        s->instance[arg_len] = '\0';
        s->filter_instance = true;
    }
    write_status_prefix(s, true);
    write_text(s, "Active instance: ");
    write_text(s, s->instance);
    return write_text(s, "\r\n");
}

static bool write_reply(mm_action_shell *s, bool ok, const char *data, size_t len)
{
    if (!len) {
        write_status_prefix(s, ok);
        if (!ok) write_text(s, "Action failed.");
        return write_text(s, "\r\n");
    }
    size_t pos = 0;
    while (pos < len) {
        size_t nl = pos;
        while (nl < len && data[nl] != '\n') nl++;
        size_t line_len = nl - pos;
        const char *line = data + pos;
        if (line_len >= 2u && (line[0] == '+' || line[0] == '-') && line[1] == ' ') {
            write_status_prefix(s, line[0] == '+');
            write_bytes(s, line + 2u, line_len - 2u);
        } else {
            write_status_prefix(s, ok);
            write_bytes(s, line, line_len);
        }
        if (nl < len) {
            write_bytes(s, "\n", 1u);
            pos = nl + 1u;
        } else {
            write_text(s, "\r\n");
            pos = len;
        }
    }
    return s->active;
}

static bool submit(mm_action_shell *s)
{
    s->esc_state = 0;
    s->hist_pos = 0;
    if (s->echo && !write_text(s, "\r\n")) return false;
    size_t start = 0, end = s->len;
    while (start < end && is_space(s->line[start])) start++;
    while (end > start && is_space(s->line[end - 1u])) end--;
    size_t trimmed_len = end - start;
    const char *trimmed = s->line + start;

    if (s->discard) {
        write_status_line(s, false, "Invalid or overlong line.");
    } else if (word_eq(trimmed, trimmed_len, "exit") || word_eq(trimmed, trimmed_len, "quit")) {
        s->active = false;
        return false;
    } else if (trimmed_len) {
        history_push(s, trimmed, trimmed_len);
        if (word_eq(trimmed, trimmed_len, "clear")) {
            write_text(s, "\x1b[2J\x1b[H");
        } else if (word_eq(trimmed, trimmed_len, "instance") ||
                   (trimmed_len > 8u && memcmp(trimmed, "instance", 8u) == 0 &&
                    is_space(trimmed[8u]))) {
            handle_instance_cmd(s, trimmed + 8u, trimmed_len - 8u);
        } else if (trimmed[trimmed_len - 1u] == '?') {
            candidates c = {.shell = s};
            mm_actions_complete(&s->view, trimmed, trimmed_len - 1u, list_candidate, &c);
            if (!c.count) write_status_line(s, false, "No matching actions.");
        } else {
            size_t dispatch_len = s->len;
            bool overlong = false;
            size_t tail = 0;
            if (s->filter_instance && is_pipe_action_line(s->line, s->len, &tail) &&
                !tail_has_instance_flag(s->line + tail, s->len - tail, s->mode)) {
                size_t inst_len = strlen(s->instance);
                if (dispatch_len + 4u + inst_len <= MM_ACTION_MAX_LINE) {
                    memcpy(s->line + dispatch_len, " -i ", 4u);
                    memcpy(s->line + dispatch_len + 4u, s->instance, inst_len);
                    dispatch_len += 4u + inst_len;
                } else {
                    overlong = true;
                }
            }
            if (overlong) {
                write_status_line(s, false, "Invalid or overlong line.");
            } else {
                s->line[dispatch_len] = '\0';
                bool ok = mm_actions_dispatch(&s->view, s->line, dispatch_len,
                                              s->mode, &s->work, &s->reply);
                if (s->reply.failed) {
                    s->active = false;
                    return false;
                }
                write_reply(s, ok, s->reply.data, s->reply.len);
                if (s->reply.truncated) write_status_line(s, false, "Reply truncated.");
            }
        }
    }
    s->len = 0;
    s->discard = false;
    return write_prompt(s);
}

bool mm_action_shell_feed(mm_action_shell *s, unsigned char byte)
{
    if (!s || !s->active) return false;
    if (byte == '\n' && s->after_cr) {
        s->after_cr = false;
        return true;
    }
    s->after_cr = byte == '\r';
    if (byte == 3) { /* Ctrl-C cancels even an overlong line or escape sequence. */
        s->discard = false;
        s->esc_state = 0;
        s->hist_pos = 0;
        s->len = 0;
        write_text(s, "^C\r\n");
        return write_prompt(s);
    }
    if (byte == '\r' || byte == '\n') return submit(s);
    if (s->discard) return true;

    if (s->esc_state == 1u) {
        if (byte == '[') {
            s->esc_state = 2u;
            s->csi_has_param = false;
        } else if (byte == 'O') {
            s->esc_state = 3u;
        } else {
            s->esc_state = 0;
        }
        return true;
    }
    if (s->esc_state == 2u) {
        if (byte >= 0x20u && byte <= 0x3fu) {
            s->csi_has_param = true;
            return true;
        }
        s->esc_state = 0;
        if (!s->csi_has_param && byte == 'A') return history_move(s, 1);
        if (!s->csi_has_param && byte == 'B') return history_move(s, -1);
        return true;
    }
    if (s->esc_state == 3u) {
        s->esc_state = 0;
        if (byte == 'A') return history_move(s, 1);
        if (byte == 'B') return history_move(s, -1);
        return true;
    }

    if (byte == 27u) {
        s->esc_state = 1u;
        return true;
    }
    if (byte == 4 && !s->len) {
        s->active = false;
        return false;
    }
    if (byte == '\t') return complete(s);
    if (byte == 8 || byte == 127) {
        if (s->len) {
            s->len--;
            s->hist_pos = 0;
            if (s->echo) return write_text(s, "\b \b");
        }
        return true;
    }
    if (byte < 32 || s->len == MM_ACTION_MAX_LINE) {
        s->discard = true;
        return true;
    }
    s->hist_pos = 0;
    s->line[s->len++] = (char)byte;
    return !s->echo || write_bytes(s, s->line + s->len - 1u, 1);
}
