// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/pipe_actions.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *values[MM_PIPE_ACTION_MAX_VALUES];
    size_t count;
} selector;

typedef struct {
    selector connector, metric, location, instance;
    const char *destination;
} selection;

static bool fail(mm_action_output *out, const char *s)
{
    mm_action_puts(out, s);
    return false;
}

static bool is_flag(const char *word)
{
    return word[0] == '-';
}

static bool has_target_flag(const mm_action_call *call)
{
    for (size_t i = 0; i < call->argc; i++) {
        if (!strcmp(call->argv[i], "-I") || !strcmp(call->argv[i], "--target-instance"))
            return true;
    }
    return false;
}

static bool is_destination_hint(const mm_pipe_action_config *cfg, const char *word)
{
    for (size_t i = 0; i < cfg->destination_count; i++) {
        if (!strcmp(word, cfg->destinations[i].name)) return true;
    }
    return (!strncmp(word, "peer:", 5) && word[5]) || (!strncmp(word, "api:", 4) && word[4]);
}

static bool parse(const mm_pipe_action_config *cfg, const mm_action_call *call,
                  mm_pipe_action_kind op, selection *s)
{
    bool explicit_target = op == MM_PIPE_COPY && has_target_flag(call);
    for (size_t i = 0; i < call->argc; ) {
        const char *word = call->argv[i++];
        selector *field = NULL;
        if (!strcmp(word, "-c") || !strcmp(word, "--connector-keys")) field = &s->connector;
        else if (!strcmp(word, "-m") || !strcmp(word, "--metric-keys")) field = &s->metric;
        else if (!strcmp(word, "-l") || !strcmp(word, "--location-keys")) field = &s->location;
        else if (!strcmp(word, "-i") || !strcmp(word, "--mrsm-instance") || !strcmp(word, "--instance")) field = &s->instance;
        else if (op == MM_PIPE_COPY && (!strcmp(word, "-I") || !strcmp(word, "--target-instance"))) {
            if (s->destination || i == call->argc || !*call->argv[i] || is_flag(call->argv[i]))
                return false;
            s->destination = call->argv[i++];
            continue;
        } else if (op == MM_PIPE_COPY && !s->destination && *word && !is_flag(word)) {
            s->destination = word;
            continue;
        } else return false;

        size_t added = 0;
        while (i < call->argc && !is_flag(call->argv[i])) {
            if (added > 0 && op == MM_PIPE_COPY && !s->destination && !explicit_target &&
                is_destination_hint(cfg, call->argv[i]))
                break;
            const char *val = call->argv[i];
            if (!*val && field != &s->location) return false;
            if (field->count == MM_PIPE_ACTION_MAX_VALUES) return false;
            field->values[field->count++] = val;
            added++;
            i++;
        }
        if (!added) return false;
    }
    return op != MM_PIPE_COPY || s->destination;
}

/* mm_pipe_actions_register validates every stream key in this immutable table. */
typedef struct {
    char key[MM_KEY_MAX + 1u];
    const char *metric, *location;
} identity;

static void identify(const mm_action_pipe *p, identity *id)
{
    const mm_stream *s = &p->pipe->target.stream;
    memcpy(id->key, s->key, s->key_len);
    id->key[s->key_len] = '\0';
    char *metric = strchr(id->key, '\x1f');
    *metric++ = '\0';
    char *location = strchr(metric, '\x1f');
    *location++ = '\0';
    id->metric = metric;
    id->location = location;
}

static bool match_token(const char *value, const char *token, bool is_location)
{
    if (is_location) {
        if (!*token || !strcmp(token, "[None]")) return !*value;
        if (!strcmp(token, "None")) return !*value || !strcmp(value, "None");
    }
    return !strcmp(value, token);
}

static bool match_selector(const selector *sel, const char *value, bool is_location)
{
    bool has_positive = false, found = false;
    for (size_t i = 0; i < sel->count; i++) {
        const char *token = sel->values[i];
        if (token[0] == '_') {
            if (match_token(value, token + 1, is_location)) return false;
        } else {
            has_positive = true;
            if (match_token(value, token, is_location)) found = true;
        }
    }
    return !has_positive || found;
}

static bool selected(const selection *s, const mm_action_pipe *p, const identity *id)
{
    return match_selector(&s->connector, id->key, false) &&
           match_selector(&s->metric, id->metric, false) &&
           match_selector(&s->location, id->location, true) &&
           match_selector(&s->instance, p->instance, false);
}

static bool admitted(const mm_pipe_action_config *cfg, const mm_action_view *v,
                     const mm_action_pipe *p, mm_pipe_action_kind op,
                     const mm_action_destination *dest)
{
    return cfg->admit ? cfg->admit(v, p, op, dest, cfg->user) : v->local;
}

static void print_quoted(mm_action_output *out, const char *s)
{
    char q = (strchr(s, '\'') && !strchr(s, '"')) ? '"' : '\'';
    mm_action_write(out, &q, 1);
    for (; *s; s++) {
        if (*s == '\\' || *s == q) mm_action_puts(out, "\\");
        mm_action_write(out, s, 1);
    }
    mm_action_write(out, &q, 1);
}

static void print_pipe(mm_action_output *out, const identity *id, const mm_action_pipe *p)
{
    mm_action_puts(out, "Pipe(");
    print_quoted(out, id->key);
    mm_action_puts(out, ", ");
    print_quoted(out, id->metric);
    if (*id->location) {
        mm_action_puts(out, ", ");
        print_quoted(out, id->location);
    }
    if (strcmp(p->instance, "journal:local") != 0) {
        mm_action_puts(out, ", instance=");
        print_quoted(out, p->instance);
    }
    mm_action_puts(out, ")");
}

static bool sync_one(const mm_pipe_action_config *cfg, const mm_action_pipe *p,
                     const identity *id, mm_action_output *out)
{
    uint64_t now = cfg->mono_us ? cfg->mono_us(cfg->user) : 0;
    mm_sync_result r = mm_pipe_sync_source(p->pipe, now, cfg->boot);
    bool ok = r.status == MM_JOURNAL_OK;
    mm_action_puts(out, ok ? "+ " : "- ");
    print_pipe(out, id, p);
    char text[104];
    snprintf(text, sizeof text, "  %zu row%s confirmed", r.confirmed, r.confirmed == 1 ? "" : "s");
    mm_action_puts(out, text);
    if (r.status == MM_JOURNAL_UNCERTAIN && r.uncertain_count) {
        snprintf(text, sizeof text, ", reconcile %zu rows from sequence %" PRIu64,
                 r.uncertain_count, r.uncertain_first);
        mm_action_puts(out, text);
    }
    if (!ok) {
        snprintf(text, sizeof text, " (sync status %d)", (int)r.status);
        mm_action_puts(out, text);
    }
    mm_action_puts(out, "\n");
    return ok;
}

static bool copy_one(const mm_action_destination *d, const mm_action_pipe *p,
                     const identity *id, mm_action_output *out)
{
    mm_send_result r = {0};
    mm_journal_status status = d->deliver(p, &r, d->user);
    bool ok = status == MM_JOURNAL_OK;
    mm_action_puts(out, ok ? "+ " : "- ");
    print_pipe(out, id, p);
    char text[96];
    snprintf(text, sizeof text, "  %zu batches, confirmed through %" PRIu64, r.batches, r.through);
    mm_action_puts(out, text);
    if (!ok) {
        snprintf(text, sizeof text, " (delivery status %d)", (int)status);
        mm_action_puts(out, text);
    }
    mm_action_puts(out, "\n");
    return ok;
}

static bool operate(mm_pipe_action_kind op, const mm_action_call *call, mm_action_output *out, void *ctx)
{
    const mm_pipe_action_config *cfg = ((mm_pipe_actions *)ctx)->config;
    selection s = {0};
    if (!parse(cfg, call, op, &s)) return fail(out, "Invalid Pipe action arguments.");
    const mm_action_destination *dest = NULL;
    if (s.destination) {
        for (size_t i = 0; i < cfg->destination_count; i++) {
            if (!strcmp(s.destination, cfg->destinations[i].name)) dest = &cfg->destinations[i];
        }
        if (!dest) return fail(out, "Unknown destination.");
    }
    bool filtered = s.connector.count || s.metric.count || s.location.count || s.instance.count;
    size_t found = 0, sources = 0;
    identity id;
    for (size_t i = 0; i < cfg->pipe_count; i++) {
        const mm_action_pipe *p = &cfg->pipes[i];
        if (!admitted(cfg, call->view, p, MM_PIPE_SHOW, NULL)) continue;
        identify(p, &id);
        if (!selected(&s, p, &id)) continue;
        found++;
        if (op == MM_PIPE_SHOW) {
            print_pipe(out, &id, p);
            mm_action_puts(out, "\n");
            continue;
        }
        if (op == MM_PIPE_SYNC && !p->pipe->fetch) {
            if (filtered) return fail(out, "Selected Pipe has no source.");
            continue;
        }
        if (!admitted(cfg, call->view, p, op, dest)) return fail(out, "Pipe operation denied.");
        sources++;
    }
    if (!found) {
        mm_action_puts(out, "No matching Pipes.");
        return op == MM_PIPE_SHOW;
    }
    if (op == MM_PIPE_SHOW) return true;
    if (op == MM_PIPE_SYNC && !sources) {
        mm_action_puts(out, "No configured source Pipes.");
        return true;
    }

    /* All matching operations were admitted before the first side effect.
     * Recheck immediately before use in case session authority was revoked. */
    bool all_ok = true;
    for (size_t i = 0; i < cfg->pipe_count; i++) {
        const mm_action_pipe *p = &cfg->pipes[i];
        if (!admitted(cfg, call->view, p, MM_PIPE_SHOW, NULL)) continue;
        identify(p, &id);
        if (!selected(&s, p, &id) || (op == MM_PIPE_SYNC && !p->pipe->fetch)) continue;
        if (!admitted(cfg, call->view, p, op, dest)) return fail(out, "Pipe operation denied.");
        if (op == MM_PIPE_SYNC) {
            if (!sync_one(cfg, p, &id, out)) all_ok = false;
        } else if (!copy_one(dest, p, &id, out)) {
            all_ok = false;
        }
    }
    return all_ok;
}

static bool show(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    return operate(MM_PIPE_SHOW, call, out, ctx);
}

static bool sync_pipes(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    return operate(MM_PIPE_SYNC, call, out, ctx);
}

static bool copy_pipes(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    return operate(MM_PIPE_COPY, call, out, ctx);
}

static bool valid_text(const char *text)
{
    if (!text || !*text) return false;
    for (; *text; text++) {
        if ((unsigned char)*text < 32 || *text == 127) return false;
    }
    return true;
}

static bool valid_config(const mm_pipe_action_config *cfg)
{
    if (!cfg || (!cfg->pipes && cfg->pipe_count) || (!cfg->destinations && cfg->destination_count))
        return false;
    for (size_t i = 0; i < cfg->pipe_count; i++) {
        const mm_action_pipe *p = &cfg->pipes[i];
        if (!p->pipe || !p->pipe->target.append || !valid_text(p->instance)) return false;
        mm_stream s = p->pipe->target.stream;
        if (!s.key || mm_stream_check(&s) != MM_OK) return false;
        for (size_t j = 0; j < s.key_len; j++) {
            unsigned char c = (unsigned char)s.key[j];
            if ((c < 32 && c != 31) || c == 127) return false;
        }
        for (size_t j = 0; j < i; j++) {
            mm_stream other = cfg->pipes[j].pipe->target.stream;
            if (!strcmp(p->instance, cfg->pipes[j].instance) && other.key_len == s.key_len &&
                !memcmp(s.key, other.key, s.key_len)) return false;
        }
    }
    for (size_t i = 0; i < cfg->destination_count; i++) {
        const mm_action_destination *d = &cfg->destinations[i];
        if (!valid_text(d->name) || d->name[0] == '-' || !d->deliver) return false;
        for (size_t j = 0; j < i; j++) {
            if (!strcmp(d->name, cfg->destinations[j].name)) return false;
        }
    }
    return true;
}

bool mm_pipe_actions_register(mm_actions *r, mm_pipe_actions *bindings, const mm_pipe_action_config *cfg)
{
    if (!r || !bindings || !valid_config(cfg)) return false;
    mm_pipe_actions next = {cfg, {
        {"show pipes", "show pipe", "List matching configured Pipes.", show, bindings},
        {"sync pipes", "sync pipe", "Fetch and commit rows from matching source Pipes.", sync_pipes, bindings},
        {"copy pipes", "copy pipe",
         "copy pipes [-I] DESTINATION [-c CONNECTOR] [-m METRIC] [-l LOCATION] [-i INSTANCE]",
         copy_pipes, bindings}
    }};
    size_t start = r->count;
    for (size_t i = 0; i < 3; i++) {
        if (!mm_actions_register(r, &next.descriptors[i])) {
            while (r->count > start) r->slots[--r->count] = NULL;
            return false;
        }
    }
    *bindings = next;
    for (size_t i = 0; i < 3; i++) r->slots[start + i] = &bindings->descriptors[i];
    return true;
}
