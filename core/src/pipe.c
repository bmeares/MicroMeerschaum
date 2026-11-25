// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include "micromeerschaum/pipe.h"

mm_status mm_pipe_key(char *out, size_t cap, size_t *used, const char *connector,
                      const char *metric, const char *location)
{
    if (!used) return MM_BAD_FIELD;
    *used = 0;
    if (!connector || !metric) return MM_BAD_KEY;
    const char *parts[] = {connector, metric, location ? location : ""};
    char key[MM_KEY_MAX];
    size_t len = 0;
    for (size_t i = 0; i < 3; ++i) {
        size_t n = strlen(parts[i]);
        if (i) {
            if (len == sizeof key) return MM_BAD_KEY;
            key[len++] = '\x1f';
        }
        if (n > sizeof key - len) return MM_BAD_KEY;
        memcpy(key + len, parts[i], n);
        len += n;
    }
    if (mm_key_check(key, len) != MM_OK) return MM_BAD_KEY;
    *used = len;
    if (!out || cap < len) return MM_BAD_LENGTH;
    memcpy(out, key, len);
    return MM_OK;
}

mm_journal_status mm_pipe_open(mm_pipe *out, const char *connector, const char *metric,
                               const char *location, mm_pipe_target target,
                               uint8_t *scratch, size_t capacity)
{
    if (!out) return MM_JOURNAL_INVALID;
    *out = (mm_pipe){0};
    if (!target.append || !target.stream.key || mm_stream_check(&target.stream) != MM_OK ||
        (!scratch && capacity)) return MM_JOURNAL_INVALID;
    char key[MM_KEY_MAX];
    size_t len;
    if (mm_pipe_key(key, sizeof key, &len, connector, metric, location) != MM_OK)
        return MM_JOURNAL_INVALID;
    if (len != target.stream.key_len || memcmp(key, target.stream.key, len))
        return MM_JOURNAL_CONFLICT;
    out->target = target;
    out->scratch = scratch;
    out->capacity = capacity;
    return MM_JOURNAL_OK;
}

static void commit(mm_pipe *pipe, const mm_sample *group, size_t n, mm_sync_result *result)
{
    uint64_t first = 0;
    result->status = pipe->target.append(pipe->target.context, group, n, &first);
    if (result->status == MM_JOURNAL_OK) {
        result->confirmed += n;
    } else if (result->status == MM_JOURNAL_UNCERTAIN && first) {
        result->uncertain_first = first;
        result->uncertain_count = n;
    }
}

mm_sync_result mm_pipe_sync_rows(mm_pipe *pipe, const mm_row *rows, size_t count,
                                 uint64_t mono_us, uint32_t boot)
{
    mm_sync_result result = {MM_JOURNAL_INVALID, 0, 0, 0};
    if (!pipe || !pipe->target.append || (!rows && count)) return result;
    result.status = MM_JOURNAL_OK;
    while (result.confirmed < count) {
        mm_sample group[8];
        size_t n = 0, at = 0;
        while (n < sizeof group / sizeof group[0] && n < count - result.confirmed) {
            mm_row row = rows[result.confirmed + n];
            size_t used;
            uint8_t *dest = pipe->scratch ? pipe->scratch + at : NULL;
            mm_status st = mm_document_encode(dest, pipe->capacity - at, row, &used);
            if ((st == MM_BAD_LENGTH && used) || (!dest && st == MM_OK)) {
                if (n) break;
                result.status = MM_JOURNAL_TOO_SMALL;
                return result;
            }
            if (st != MM_OK) {
                result.status = MM_JOURNAL_INVALID;
                return result;
            }
            group[n++] = (mm_sample){MM_KIND_DATA, mono_us, boot, dest, (uint32_t)used};
            at += used;
        }
        commit(pipe, group, n, &result);
        if (result.status != MM_JOURNAL_OK) break;
    }
    return result;
}

mm_journal_status mm_pipe_bind_source(mm_pipe *pipe, const char *name, mm_pipe_fetch_fn fetch,
                                      void *context)
{
    if (!pipe || !pipe->target.append || !name || !*name || !fetch) return MM_JOURNAL_INVALID;
    mm_stream stream = pipe->target.stream;
    const char *end = memchr(stream.key, '\x1f', stream.key_len);
    const size_t prefix = sizeof "plugin:" - 1;
    if (!end || (size_t)(end - stream.key) <= prefix ||
        (size_t)(end - stream.key) - prefix != strlen(name) ||
        memcmp(stream.key, "plugin:", prefix) || memcmp(stream.key + prefix, name, strlen(name)))
        return MM_JOURNAL_INVALID;
    pipe->fetch = fetch;
    pipe->source_context = context;
    return MM_JOURNAL_OK;
}

mm_sync_result mm_pipe_sync_source(mm_pipe *pipe, uint64_t mono_us, uint32_t boot)
{
    mm_sync_result result = {MM_JOURNAL_INVALID, 0, 0, 0};
    if (!pipe || !pipe->fetch) return result;
    for (;;) {
        bool done = false;
        mm_row row = {NULL, 0};
        result.status = pipe->fetch(pipe->source_context, pipe, result.confirmed, &row, &done);
        if (result.status != MM_JOURNAL_OK || done) return result;
        if (result.confirmed == SIZE_MAX) {
            result.status = MM_JOURNAL_FULL;
            return result;
        }
        mm_sync_result write = mm_pipe_sync_rows(pipe, &row, 1, mono_us, boot);
        write.confirmed += result.confirmed;
        result = write;
        if (result.status != MM_JOURNAL_OK) return result;
    }
}

mm_sync_result mm_pipe_sync_binary(mm_pipe *pipe, const void *payload, size_t len,
                                   uint64_t mono_us, uint32_t boot)
{
    mm_sync_result result = {MM_JOURNAL_INVALID, 0, 0, 0};
    if (!pipe || !pipe->target.append || (!payload && len) || len > MM_RECORD_MAX_PAYLOAD)
        return result;
    const mm_sample sample = {MM_KIND_DATA, mono_us, boot, payload, (uint32_t)len};
    commit(pipe, &sample, 1, &result);
    return result;
}
