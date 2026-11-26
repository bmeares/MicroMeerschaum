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
    mm_pipe_data_request data;
    bool has_limit;
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

static bool is_bound_value(const char *word)
{
    if (!word || !*word) return false;
    if (word[0] == '-') return word[1] >= '0' && word[1] <= '9';
    return true;
}

static bool parse_limit(const char *word, size_t *out)
{
    if (!word || !*word) return false;
    size_t val = 0;
    for (const char *p = word; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        val = val * 10u + (size_t)(*p - '0');
        if (val > MM_PIPE_DATA_MAX_LIMIT) return false;
    }
    *out = val;
    return true;
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
    s->data.limit = MM_PIPE_DATA_DEFAULT_LIMIT;
    bool explicit_target = op == MM_PIPE_COPY && has_target_flag(call);
    for (size_t i = 0; i < call->argc; ) {
        const char *word = call->argv[i++];
        selector *field = NULL;
        if (!strcmp(word, "-c") || !strcmp(word, "--connector-keys")) field = &s->connector;
        else if (!strcmp(word, "-m") || !strcmp(word, "--metric-keys")) field = &s->metric;
        else if (!strcmp(word, "-l") || !strcmp(word, "--location-keys")) field = &s->location;
        else if (!strcmp(word, "-i") || !strcmp(word, "--mrsm-instance") || !strcmp(word, "--instance")) field = &s->instance;
        else if (op == MM_PIPE_DATA && !strcmp(word, "--limit")) {
            if (s->has_limit || i == call->argc || !parse_limit(call->argv[i], &s->data.limit))
                return false;
            s->has_limit = true;
            i++;
            continue;
        } else if (op == MM_PIPE_DATA && !strcmp(word, "--begin")) {
            if (s->data.begin || i == call->argc || !is_bound_value(call->argv[i]))
                return false;
            s->data.begin = call->argv[i++];
            continue;
        } else if (op == MM_PIPE_DATA && !strcmp(word, "--end")) {
            if (s->data.end || i == call->argc || !is_bound_value(call->argv[i]))
                return false;
            s->data.end = call->argv[i++];
            continue;
        } else if (op == MM_PIPE_COPY && (!strcmp(word, "-I") || !strcmp(word, "--target-instance"))) {
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

static bool inspect_pipes(mm_pipe_action_kind op, const mm_action_call *call,
                          mm_action_output *out, void *ctx)
{
    const mm_pipe_inspect_actions *b = ctx;
    const mm_pipe_action_config *cfg = b->pipes;
    const mm_pipe_inspect_config *ins = b->inspect;
    selection s = {0};
    if (!parse(cfg, call, op, &s)) return fail(out, "Invalid Pipe action arguments.");

    size_t found = 0;
    identity id;
    for (size_t i = 0; i < cfg->pipe_count; i++) {
        const mm_action_pipe *p = &cfg->pipes[i];
        if (!admitted(cfg, call->view, p, MM_PIPE_SHOW, NULL)) continue;
        identify(p, &id);
        if (!selected(&s, p, &id)) continue;
        found++;
        if (!admitted(cfg, call->view, p, op, NULL)) return fail(out, "Pipe operation denied.");
    }
    if (!found) {
        mm_action_puts(out, "No matching Pipes.");
        return true;
    }

    bool all_ok = true;
    for (size_t i = 0; i < cfg->pipe_count; i++) {
        const mm_action_pipe *p = &cfg->pipes[i];
        if (!admitted(cfg, call->view, p, MM_PIPE_SHOW, NULL)) continue;
        identify(p, &id);
        if (!selected(&s, p, &id)) continue;
        if (!admitted(cfg, call->view, p, op, NULL)) return fail(out, "Pipe operation denied.");
        char text[128];
        if (op == MM_PIPE_DATA) {
            print_pipe(out, &id, p);
            mm_action_puts(out, "\n");
            if (!ins->data(p, &s.data, out, ins->user)) all_ok = false;
        } else if (op == MM_PIPE_COLUMNS) {
            print_pipe(out, &id, p);
            mm_action_puts(out, "\n");
            if (!ins->columns(p, out, ins->user)) all_ok = false;
        } else if (op == MM_PIPE_ROWCOUNT) {
            uint64_t count = 0;
            if (ins->rowcount(p, &count, ins->user)) {
                print_pipe(out, &id, p);
                snprintf(text, sizeof text, "  %" PRIu64 "\n", count);
                mm_action_puts(out, text);
            } else {
                all_ok = false;
                mm_action_puts(out, "- ");
                print_pipe(out, &id, p);
                mm_action_puts(out, "  rowcount unavailable\n");
            }
        } else if (op == MM_PIPE_PROGRESS) {
            mm_pipe_progress prog = {0};
            if (ins->progress(p, &prog, ins->user)) {
                print_pipe(out, &id, p);
                snprintf(text, sizeof text,
                         "  prefix=%" PRIu64 " high=%" PRIu64 " confirmed=%" PRIu64 " missed=%" PRIu64 "\n",
                         prog.prefix, prog.high, prog.confirmed, prog.missed);
                mm_action_puts(out, text);
            } else {
                all_ok = false;
                mm_action_puts(out, "- ");
                print_pipe(out, &id, p);
                mm_action_puts(out, "  progress unavailable\n");
            }
        }
        if (out->truncated) break;
    }
    return all_ok;
}

static bool show_data(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    return inspect_pipes(MM_PIPE_DATA, call, out, ctx);
}

static bool show_columns(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    return inspect_pipes(MM_PIPE_COLUMNS, call, out, ctx);
}

static bool show_rowcounts(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    return inspect_pipes(MM_PIPE_ROWCOUNT, call, out, ctx);
}

static bool show_progress(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    return inspect_pipes(MM_PIPE_PROGRESS, call, out, ctx);
}

static bool valid_connector_type(const char *word)
{
    return !strcmp(word, "journal") || !strcmp(word, "sql") ||
           !strcmp(word, "peer") || !strcmp(word, "api");
}

static bool connector_matches_type(const char *name, const char *filter)
{
    if (!filter) return true;
    size_t len = strlen(filter);
    return !strncmp(name, filter, len) && name[len] == ':';
}

static bool seen_instance_before(const mm_pipe_action_config *cfg, const mm_action_view *view,
                                 size_t limit, const char *name)
{
    for (size_t i = 0; i < limit; i++) {
        const mm_action_pipe *p = &cfg->pipes[i];
        if (!admitted(cfg, view, p, MM_PIPE_SHOW, NULL)) continue;
        if (!strcmp(p->instance, name)) return true;
    }
    return false;
}

static bool show_connectors(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    const mm_pipe_action_config *cfg = ((const mm_pipe_inspect_actions *)ctx)->pipes;
    const char *filter = NULL;
    if (call->argc > 1) return fail(out, "Invalid Pipe action arguments.");
    if (call->argc == 1) {
        if (!valid_connector_type(call->argv[0])) return fail(out, "Invalid Pipe action arguments.");
        filter = call->argv[0];
    }
    size_t emitted = 0;
    for (size_t i = 0; i < cfg->pipe_count; i++) {
        const mm_action_pipe *p = &cfg->pipes[i];
        if (!admitted(cfg, call->view, p, MM_PIPE_SHOW, NULL)) continue;
        if (!connector_matches_type(p->instance, filter)) continue;
        if (seen_instance_before(cfg, call->view, i, p->instance)) continue;
        mm_action_puts(out, p->instance);
        mm_action_puts(out, "\n");
        emitted++;
    }
    for (size_t i = 0; i < cfg->destination_count; i++) {
        const char *name = cfg->destinations[i].name;
        if (!connector_matches_type(name, filter)) continue;
        if (seen_instance_before(cfg, call->view, cfg->pipe_count, name)) continue;
        mm_action_puts(out, name);
        mm_action_puts(out, "\n");
        emitted++;
    }
    if (!emitted) mm_action_puts(out, "No matching connectors.");
    return true;
}

static bool show_version(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    const mm_pipe_inspect_config *ins = ((const mm_pipe_inspect_actions *)ctx)->inspect;
    if (call->argc) return fail(out, "Usage: show version");
    const char *label = (ins && ins->version) ? ins->version
                                              : "MicroMeerschaum " MM_VERSION_STRING " (native)";
    mm_action_puts(out, label);
    mm_action_puts(out, "\n");
    return true;
}

bool mm_pipe_inspect_register(mm_actions *r, mm_pipe_inspect_actions *bindings,
                              const mm_pipe_action_config *pipes,
                              const mm_pipe_inspect_config *inspect)
{
    if (!r || !bindings || !valid_config(pipes)) return false;
    if (inspect && inspect->version && !valid_text(inspect->version)) return false;

    mm_pipe_inspect_actions next = {.pipes = pipes, .inspect = inspect, .count = 0};
    if (inspect && inspect->data) {
        next.descriptors[next.count++] = (mm_action){
            "show data", "show datum",
            "show data [-c CONNECTOR] [-m METRIC] [-l LOCATION] [-i INSTANCE] [--limit N] [--begin VAL] [--end VAL]",
            show_data, bindings
        };
    }
    if (inspect && inspect->columns) {
        next.descriptors[next.count++] = (mm_action){
            "show columns", "show column",
            "Show declared columns and types for matching Pipes.",
            show_columns, bindings
        };
    }
    if (inspect && inspect->rowcount) {
        next.descriptors[next.count++] = (mm_action){
            "show rowcounts", "show rowcount",
            "Show local row counts for matching Pipes.",
            show_rowcounts, bindings
        };
    }
    if (inspect && inspect->progress) {
        next.descriptors[next.count++] = (mm_action){
            "show progress", NULL,
            "Show delivery cursor state for matching Pipes.",
            show_progress, bindings
        };
    }
    next.descriptors[next.count++] = (mm_action){
        "show connectors", "show connector",
        "List active local storage instances and provisioned destinations.",
        show_connectors, bindings
    };
    next.descriptors[next.count++] = (mm_action){
        "show version", NULL,
        "Show MicroMeerschaum version and runtime.",
        show_version, bindings
    };

    size_t start = r->count;
    for (size_t i = 0; i < next.count; i++) {
        if (!mm_actions_register(r, &next.descriptors[i])) {
            while (r->count > start) r->slots[--r->count] = NULL;
            return false;
        }
    }
    *bindings = next;
    for (size_t i = 0; i < bindings->count; i++) r->slots[start + i] = &bindings->descriptors[i];
    return true;
}
