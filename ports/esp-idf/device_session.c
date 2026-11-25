// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/device_session.h"
#include "../../core/src/utf8.h"
#include "micromeerschaum/document.h"
#include "cJSON.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PREFIX_LEN (sizeof MM_SESSION_PREFIX - 1u)
#define MEMBER(obj, key) cJSON_GetObjectItemCaseSensitive((obj), (key))

static bool text_key(const char *s)
{
    if (!s || !*s || !mm_utf8_valid((const uint8_t *)s, strlen(s))) return false;
    for (; *s; s++) if ((unsigned char)*s < 32 || *s == 127) return false;
    return true;
}

bool mm_device_session_init(mm_device_session *s, const mm_device_session_config *c)
{
    if (!s) return false;
    s->config = NULL;
    mm_device_session_reset(s);
    if (!c || !c->view.registry || c->view.local || (!c->pipes && c->pipe_count) ||
        (c->mode != MM_ACTION_QUOTED && c->mode != MM_ACTION_LITERAL && c->mode != MM_ACTION_LITERAL_PREFIX)) return false;
    for (size_t i = 0; i < c->pipe_count; i++) {
        const mm_session_pipe *p = &c->pipes[i];
        if (p->id <= 0 || (p->filters & ~7u) || !text_key(p->connector) || !text_key(p->metric) ||
            (p->location && !text_key(p->location)) || !text_key(p->instance)) return false;
        for (size_t j = 0; j < i; j++) {
            const mm_session_pipe *q = &c->pipes[j];
            if (p->id == q->id || (!strcmp(p->connector, q->connector) &&
                !strcmp(p->metric, q->metric) && !strcmp(p->location ? p->location : "",
                                                          q->location ? q->location : ""))) return false;
        }
    }
    s->config = c;
    return true;
}

void mm_device_session_reset(mm_device_session *s)
{
    if (s) { s->used = 0; s->discard = false; }
}

static bool digit(char c) { return c >= '0' && c <= '9'; }

static bool number_end(const char *s, size_t n, size_t *at, bool *precise)
{
    size_t i = *at;
    if (s[i] == '-') i++;
    if (i == n || !digit(s[i])) return false;
    size_t first = i;
    if (s[i++] == '0' && i < n && digit(s[i])) return false;
    while (i < n && digit(s[i])) i++;
    bool integer = true;
    if (i < n && s[i] == '.') {
        integer = false;
        if (++i == n || !digit(s[i])) return false;
        while (i < n && digit(s[i])) i++;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        integer = false;
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        if (i == n || !digit(s[i])) return false;
        while (i < n && digit(s[i])) i++;
    }
    /* cJSON uses doubles for numbers. Never round a decimal integer before a write. */
    if (integer && (i - first > 16 || (i - first == 16 && memcmp(s + first, "9007199254740991", 16) > 0)))
        *precise = false;
    if (i < n && !strchr(" \t\r,]}", s[i])) return false;
    *at = i - 1;
    return true;
}

/* cJSON owns structural and Unicode escape decoding. Check the parts of
 * RFC 8259 it accepts more loosely before allocating or entering a handler. */
static bool wire_json(const char *s, size_t n, bool *precise)
{
    if (!mm_utf8_valid((const uint8_t *)s, n)) return false;
    bool string = false;
    unsigned depth = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if ((unsigned char)c < 32 && (string || (c != '\t' && c != '\r'))) return false;
        if (c == '"') string = !string;
        else if (string && c == '\\') {
            if (++i == n || !strchr("\"\\/bfnrtu", s[i]) || !s[i]) return false;
            if (s[i] == 'u') {
                if (n - i < 5 || !memcmp(s + i + 1, "0000", 4)) return false;
                for (size_t j = 1; j <= 4; j++) {
                    char h = s[i + j];
                    if (!digit(h) && !(h >= 'A' && h <= 'F') && !(h >= 'a' && h <= 'f')) return false;
                }
                i += 4;
            }
        } else if (!string) {
            if (c == '{' || c == '[') { if (++depth > 8) return false; }
            else if (c == '}' || c == ']') { if (!depth) return false; depth--; }
            else if (c == '-' || digit(c)) { if (!number_end(s, n, &i, precise)) return false; }
            else if (!strchr(" \t\r:,truefalsn", c)) return false;
        }
    }
    return !string && !depth;
}

static bool unambiguous(const cJSON *j)
{
    if (cJSON_IsNumber(j) && !isfinite(j->valuedouble)) return false;
    for (const cJSON *v = j->child; v; v = v->next) {
        if (v->string) {
            for (const cJSON *other = v->next; other; other = other->next)
                if (!strcmp(v->string, other->string)) return false;
        }
        if (!unambiguous(v)) return false;
    }
    return true;
}

static bool valid_id(const char *id)
{
    if (!id || !*id || strlen(id) > 32) return false;
    for (; *id; id++)
        if (!digit(*id) && !(*id >= 'A' && *id <= 'Z') && !(*id >= 'a' && *id <= 'z')) return false;
    return true;
}

static bool integer(const cJSON *v, int max, int *out)
{
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= max) ||
        (double)(int)v->valuedouble != v->valuedouble) return false;
    *out = (int)v->valuedouble;
    return true;
}

static bool scalar(const cJSON *v)
{
    if (cJSON_IsNull(v) || cJSON_IsBool(v) || cJSON_IsString(v)) return true;
    if (cJSON_IsNumber(v)) return isfinite(v->valuedouble);
    if (!cJSON_IsObject(v) || !v->child || v->child->next || strcmp(v->child->string, "$us")) return false;
    const cJSON *num = v->child;
    return cJSON_IsNumber(num) && fabs(num->valuedouble) <= 9007199254740991.0 &&
           trunc(num->valuedouble) == num->valuedouble;
}

static bool document(const cJSON *row)
{
    if (!cJSON_IsObject(row) || cJSON_GetArraySize(row) > (int)MM_DOCUMENT_MAX_FIELDS) return false;
    for (const cJSON *v = row->child; v; v = v->next)
        if (!*v->string || !scalar(v)) return false;
    return true;
}

static bool rows_valid(const cJSON *rows, int limit)
{
    if (!cJSON_IsArray(rows)) return false;
    for (const cJSON *row = rows->child; row; row = row->next)
        if (limit-- <= 0 || !document(row)) return false;
    return true;
}

static bool allowed(const mm_device_session_config *c, const mm_session_pipe *p, mm_session_access op)
{
    return c->allow_pipe && c->allow_pipe(p, op, c->user);
}

static bool fail(mm_action_output *out, const char *s)
{
    mm_action_puts(out, s);
    return false;
}

static bool append(cJSON *array, cJSON *item)
{
    if (item && cJSON_AddItemToArray(array, item)) return true;
    cJSON_Delete(item);
    return false;
}

/* May be called while building a page to check its actual escaped wire size. */
static size_t encode(mm_device_session *s, const char *id, bool ok, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    bool built = root && cJSON_AddNumberToObject(root, "v", 1) && cJSON_AddStringToObject(root, "id", id) &&
                 cJSON_AddBoolToObject(root, "ok", ok) && cJSON_AddItemReferenceToObject(root, ok ? "data" : "error", data);
    memcpy(s->reply, MM_SESSION_PREFIX, PREFIX_LEN);
    if (built) built = cJSON_PrintPreallocated(root, s->reply + PREFIX_LEN,
                                                (int)(sizeof s->reply - PREFIX_LEN), false);
    cJSON_Delete(root);
    if (!built) return 0;
    size_t n = strlen(s->reply);
    if (n >= MM_SESSION_FRAME_MAX || !mm_utf8_valid((const uint8_t *)s->reply, n)) return 0;
    s->reply[n++] = '\n';
    return n;
}

static cJSON *catalog_item(const mm_device_session_config *c, const mm_session_pipe *p)
{
    cJSON *item = cJSON_CreateObject();
    cJSON *key = item ? cJSON_AddArrayToObject(item, "key") : NULL;
    cJSON *filters = item ? cJSON_AddArrayToObject(item, "filters") : NULL;
    if (!key || !filters ||
        ((p->filters & MM_SESSION_FILTER_BEGIN) && !append(filters, cJSON_CreateString("begin"))) ||
        ((p->filters & MM_SESSION_FILTER_END) && !append(filters, cJSON_CreateString("end"))) ||
        ((p->filters & MM_SESSION_FILTER_PARAMS) && !append(filters, cJSON_CreateString("params"))) ||
        !append(key, cJSON_CreateString(p->connector)) || !append(key, cJSON_CreateString(p->metric)) ||
        !append(key, p->location ? cJSON_CreateString(p->location) : cJSON_CreateNull()) ||
        !cJSON_AddNumberToObject(item, "id", p->id) || !cJSON_AddStringToObject(item, "instance", p->instance) ||
        !cJSON_AddBoolToObject(item, "read", p->read && c->allow_op("read", c->user) && allowed(c, p, MM_SESSION_READ)) ||
        !cJSON_AddBoolToObject(item, "write", p->write && c->allow_op("write", c->user) && allowed(c, p, MM_SESSION_WRITE))) {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

static bool catalog(mm_device_session *s, const cJSON *request, const char *id, cJSON **data, mm_action_output *out)
{
    int offset = 0, limit = 16;
    const cJSON *off = MEMBER(request, "offset"), *lim = MEMBER(request, "limit");
    if ((off && !integer(off, INT_MAX, &offset)) || (lim && !integer(lim, 64, &limit)))
        return fail(out, "Invalid catalog page.");
    *data = cJSON_CreateObject();
    cJSON *items = *data ? cJSON_AddArrayToObject(*data, "items") : NULL;
    cJSON *more = items ? cJSON_AddFalseToObject(*data, "more") : NULL;
    if (!more) return fail(out, "Out of memory.");
    int count = 0;
    const mm_device_session_config *c = s->config;
    for (size_t i = 0; i < c->pipe_count; i++) {
        const mm_session_pipe *p = &c->pipes[i];
        if (!allowed(c, p, MM_SESSION_DISCOVER)) continue;
        if (offset) { offset--; continue; }
        if (count == limit) { cJSON_SetBoolValue(more, true); break; }
        if (!append(items, catalog_item(c, p))) return fail(out, "Out of memory.");
        if (!encode(s, id, true, *data)) {
            cJSON_DeleteItemFromArray(items, count);
            if (!count) return fail(out, "Catalog entry exceeds frame limit.");
            cJSON_SetBoolValue(more, true);
            break;
        }
        count++;
    }
    return true;
}

typedef struct {
    const mm_actions *registry;
    cJSON *list;
    size_t size;
    bool ok;
} action_list;

static void list_name(const char *name, void *ctx)
{
    action_list *l = ctx;
    if (!l->ok) return;
    l->size += strlen(name) + 3u;
    l->ok = l->size <= MM_SESSION_FRAME_MAX && append(l->list, cJSON_CreateString(name));
}

static void list_action(const mm_action *a, void *ctx) { list_name(a->name, ctx); }

static void list_canonical(const char *name, void *ctx)
{
    action_list *l = ctx;
    for (size_t i = 0; i < l->registry->count; i++) {
        if (name == l->registry->slots[i]->name) { list_name(name, ctx); return; }
    }
}

static bool named_actions(mm_device_session *s, const char *op, const cJSON *request,
                          cJSON **data, mm_action_output *out)
{
    const mm_action_view *v = &s->config->view;
    const char *line = cJSON_GetStringValue(MEMBER(request, "line"));
    if (strcmp(op, "actions") && !line) return fail(out, "Expected an action line.");
    if (!strcmp(op, "action")) {
        bool ok = mm_actions_dispatch(v, line, strlen(line), s->config->mode, &s->work, out);
        if (!ok) return false;
        *data = cJSON_CreateString(out->data);
        return *data != NULL;
    }
    *data = cJSON_CreateArray();
    if (!*data) return fail(out, "Out of memory.");
    action_list l = {v->registry, *data, 0, true};
    if (!strcmp(op, "actions")) mm_actions_visit(v, list_action, &l);
    else mm_actions_complete(v, line, strlen(line), list_canonical, &l);
    return l.ok || fail(out, "Action list exceeds frame limit.");
}

static bool pipe_operation(const mm_device_session_config *c, const cJSON *req, bool write,
                           cJSON **data, mm_action_output *out)
{
    const cJSON *key = MEMBER(req, "key");
    if (!cJSON_IsArray(key) || cJSON_GetArraySize(key) != 3) return fail(out, "Invalid Pipe key.");
    const char *connector = cJSON_GetStringValue(key->child), *metric = cJSON_GetStringValue(key->child->next);
    const cJSON *loc = key->child->next->next;
    const char *location = cJSON_GetStringValue(loc);
    if (!text_key(connector) || !text_key(metric) || (!cJSON_IsNull(loc) && !text_key(location)))
        return fail(out, "Invalid Pipe key.");
    const mm_session_pipe *p = NULL;
    for (size_t i = 0; i < c->pipe_count; i++) {
        const mm_session_pipe *candidate = &c->pipes[i];
        if (!strcmp(candidate->connector, connector) && !strcmp(candidate->metric, metric) &&
            !strcmp(candidate->location ? candidate->location : "", location ? location : "")) {
            p = candidate;
            break;
        }
    }
    if (!p || !allowed(c, p, MM_SESSION_DISCOVER) ||
        !allowed(c, p, write ? MM_SESSION_WRITE : MM_SESSION_READ) || (write ? !p->write : !p->read))
        return fail(out, "Pipe unavailable.");
    if (write) {
        const cJSON *rows = MEMBER(req, "rows");
        if (!rows_valid(rows, 128)) return fail(out, "Invalid write rows.");
        if (!p->write(rows, out, p->user)) return false;
        *data = cJSON_CreateString(out->data);
        return *data != NULL;
    }
    int limit;
    const cJSON *begin = MEMBER(req, "begin"), *end = MEMBER(req, "end"), *params = MEMBER(req, "params");
    if (!integer(MEMBER(req, "limit"), 128, &limit) ||
        (begin && !scalar(begin)) || (end && !scalar(end)) || (params && !cJSON_IsNull(params) && !cJSON_IsObject(params)))
        return fail(out, "Invalid read selection.");
    if ((begin && !cJSON_IsNull(begin) && !(p->filters & MM_SESSION_FILTER_BEGIN)) ||
        (end && !cJSON_IsNull(end) && !(p->filters & MM_SESSION_FILTER_END)) ||
        (params && params->child && !(p->filters & MM_SESSION_FILTER_PARAMS)))
        return fail(out, "Unsupported read selection.");
    for (const cJSON *param = params ? params->child : NULL; param; param = param->next) {
        if (!*param->string) return fail(out, "Invalid read parameters.");
        if (cJSON_IsArray(param)) {
            for (const cJSON *arg = param->child; arg; arg = arg->next)
                if (!scalar(arg)) return fail(out, "Invalid read parameters.");
        } else if (!scalar(param)) return fail(out, "Invalid read parameters.");
    }
    *data = cJSON_CreateArray();
    if (!*data) return fail(out, "Out of memory.");
    if (!p->read(req, *data, out, p->user)) return false;
    return rows_valid(*data, limit) || fail(out, "Read exceeded requested document bounds.");
}

static bool fields(const cJSON *req, const char *const *extra)
{
    for (const cJSON *v = req->child; v; v = v->next) {
        if (!strcmp(v->string, "v") || !strcmp(v->string, "id") || !strcmp(v->string, "op")) continue;
        size_t i = 0;
        while (extra[i] && strcmp(v->string, extra[i])) i++;
        if (!extra[i]) return false;
    }
    return true;
}

static bool execute(mm_device_session *s, const cJSON *req, const char *id, bool precise,
                    cJSON **data, mm_action_output *out)
{
    static const char *const page[] = {"offset", "limit", NULL}, *const line[] = {"line", NULL};
    static const char *const read[] = {"key", "limit", "begin", "end", "params", NULL};
    static const char *const write[] = {"key", "rows", NULL}, *const empty[] = {NULL};
    const char *op = cJSON_GetStringValue(MEMBER(req, "op"));
    int version;
    if (!precise || !unambiguous(req) || !integer(MEMBER(req, "v"), 1, &version) || version != 1 || !op)
        return fail(out, "Invalid or unsupported request.");
    const mm_device_session_config *c = s->config;
    if (!c->allow_op || !c->allow_op(op, c->user)) return fail(out, "Operation unavailable.");
    if (!strcmp(op, "catalog") && fields(req, page)) return catalog(s, req, id, data, out);
    if ((!strcmp(op, "actions") && fields(req, empty)) ||
        ((!strcmp(op, "action") || !strcmp(op, "complete")) && fields(req, line)))
        return named_actions(s, op, req, data, out);
    if (!strcmp(op, "read") && fields(req, read)) return pipe_operation(c, req, false, data, out);
    if (!strcmp(op, "write") && fields(req, write)) return pipe_operation(c, req, true, data, out);
    return fail(out, "Unknown operation or fields.");
}

bool mm_device_session_line(mm_device_session *s, const char *line, size_t n, mm_action_emit_fn emit, void *io)
{
    if (!s || !s->config || !line || !emit) return false;
    if (n > MM_SESSION_FRAME_MAX || n <= PREFIX_LEN || line[n - 1] != '\n' ||
        memcmp(line, MM_SESSION_PREFIX, PREFIX_LEN)) return true;
    const char *json = line + PREFIX_LEN, *end;
    size_t size = n - PREFIX_LEN - 1u;
    bool precise = true;
    if (!wire_json(json, size, &precise)) return true;
    cJSON *req = cJSON_ParseWithLengthOpts(json, size, &end, false);
    if (!req) return true;
    while (end < json + size && (*end == ' ' || *end == '\t' || *end == '\r')) end++;
    const char *id = cJSON_GetStringValue(MEMBER(req, "id"));
    if (end != json + size || !cJSON_IsObject(req) || !valid_id(id)) { cJSON_Delete(req); return true; }

    mm_action_output out;
    mm_action_output_init(&out, s->line, sizeof s->line);
    cJSON *data = NULL;
    bool ok = execute(s, req, id, precise, &data, &out);
    if (out.truncated || out.failed || memchr(out.data, 0, out.len) ||
        !mm_utf8_valid((const uint8_t *)out.data, out.len)) {
        ok = false;
        mm_action_output_init(&out, s->line, sizeof s->line);
        mm_action_puts(&out, "Reply unavailable after execution.");
    }
    if (!ok) {
        cJSON_Delete(data);
        data = cJSON_CreateString(out.len ? out.data : "Operation failed.");
    }
    size_t used = data ? encode(s, id, ok, data) : 0;
    cJSON_Delete(data);
    if (!used) {
        int length = snprintf(s->reply, sizeof s->reply,
            MM_SESSION_PREFIX "{\"v\":1,\"id\":\"%s\",\"ok\":false,\"error\":\"Reply exceeds budget or unavailable.\"}\n", id);
        used = (size_t)length;
    }
    cJSON_Delete(req);
    return emit(s->reply, used, io);
}

bool mm_device_session_feed(mm_device_session *s, const char *p, size_t n, mm_action_emit_fn emit, void *io)
{
    if (!s || !s->config || (!p && n) || !emit) return false;
    for (size_t i = 0; i < n; i++) {
        if (s->used == MM_SESSION_FRAME_MAX) s->discard = true;
        if (!s->discard) s->line[s->used++] = p[i];
        if (p[i] != '\n') continue;
        bool accepted = s->discard || mm_device_session_line(s, s->line, s->used, emit, io);
        mm_device_session_reset(s);
        if (!accepted) return false;
    }
    return true;
}
