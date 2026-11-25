// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include "le.h"
#include "micromeerschaum/gatt.h"

enum { PUSH = 1, SEEK = 2, REPLY = 3 };

mm_journal_status mm_gatt_init(mm_gatt_server *s, uint8_t *batch, size_t batch_cap,
                              uint8_t *reply, size_t reply_cap, mm_gatt_commit commit, void *ctx)
{
    if (!s || !batch || !batch_cap || !reply || !reply_cap || !commit)
        return MM_JOURNAL_INVALID;
    *s = (mm_gatt_server){.batch = batch, .batch_cap = batch_cap, .reply = reply,
                          .reply_cap = reply_cap, .commit = commit, .ctx = ctx};
    return MM_JOURNAL_OK;
}

void mm_gatt_reset(mm_gatt_server *s)
{
    s->total = s->received = s->reply_len = s->read_at = s->read_limit = 0;
}

static void frame(uint8_t *p, uint8_t type, size_t offset, size_t value)
{
    p[0] = type;
    mm_put32(p + 1, (uint32_t)offset);
    mm_put32(p + 5, (uint32_t)value);
}

mm_journal_status mm_gatt_write(mm_gatt_server *s, const uint8_t *p, size_t size)
{
    if (!s || !p || size < MM_GATT_HEADER) return MM_JOURNAL_INVALID;
    const size_t offset = mm_get32(p + 1), value = mm_get32(p + 5);
    if (p[0] == SEEK) {
        s->read_limit = 0;
        if (size != MM_GATT_HEADER || offset >= s->reply_len || value <= MM_GATT_HEADER)
            return MM_JOURNAL_INVALID;
        s->read_at = offset;
        s->read_limit = value;
        return MM_JOURNAL_OK;
    }
    if (p[0] != PUSH || size == MM_GATT_HEADER) return MM_JOURNAL_INVALID;
    if (offset == 0) {
        mm_gatt_reset(s);
        if (value > s->batch_cap) return MM_JOURNAL_TOO_SMALL;
        s->total = value;
    }
    const size_t bytes = size - MM_GATT_HEADER;
    if (!s->total || s->total != value || offset != s->received || bytes > value - offset) {
        mm_gatt_reset(s);
        return MM_JOURNAL_INVALID;
    }
    memcpy(s->batch + offset, p + MM_GATT_HEADER, bytes);
    s->received += bytes;
    if (s->received != s->total) return MM_JOURNAL_OK;
    s->total = s->received = 0;
    size_t reply_len = 0;
    mm_journal_status status = s->commit(s->ctx, s->batch, value, s->reply, s->reply_cap, &reply_len);
    if (status != MM_JOURNAL_OK) return status;
    if (!reply_len || reply_len > UINT32_MAX || reply_len > s->reply_cap) return MM_JOURNAL_INVALID;
    s->reply_len = reply_len;
    return MM_JOURNAL_OK;
}

mm_journal_status mm_gatt_read(mm_gatt_server *s, uint8_t *out, size_t cap, size_t *used)
{
    if (!used) return MM_JOURNAL_INVALID;
    *used = 0;
    if (!s || !out || !s->read_limit) return MM_JOURNAL_INVALID;
    if (cap > s->read_limit) cap = s->read_limit;
    if (cap <= MM_GATT_HEADER) return MM_JOURNAL_TOO_SMALL;
    size_t bytes = s->reply_len - s->read_at;
    if (bytes > cap - MM_GATT_HEADER) bytes = cap - MM_GATT_HEADER;
    frame(out, REPLY, s->read_at, s->reply_len);
    memcpy(out + MM_GATT_HEADER, s->reply + s->read_at, bytes);
    *used = MM_GATT_HEADER + bytes;
    return MM_JOURNAL_OK;
}

mm_journal_status mm_gatt_exchange(void *ctx, const uint8_t *data, size_t len,
                                  uint8_t *reply, size_t cap, size_t *used)
{
    if (!used) return MM_JOURNAL_INVALID;
    *used = 0;
    mm_gatt_link *link = ctx;
    if (!link || !link->read || !link->write || !link->scratch || !data || !len ||
        len > UINT32_MAX || !reply || !cap || link->value_cap <= MM_GATT_HEADER ||
        link->value_cap > UINT32_MAX) return MM_JOURNAL_INVALID;
    uint8_t *p = link->scratch;
    for (size_t at = 0; at < len;) {
        size_t bytes = len - at;
        if (bytes > link->value_cap - MM_GATT_HEADER) bytes = link->value_cap - MM_GATT_HEADER;
        frame(p, PUSH, at, len);
        memcpy(p + MM_GATT_HEADER, data + at, bytes);
        mm_journal_status st = link->write(link->ctx, p, MM_GATT_HEADER + bytes);
        if (st != MM_JOURNAL_OK) return st;
        at += bytes;
    }
    size_t at = 0, total = 0;
    do {
        frame(p, SEEK, at, link->value_cap);
        mm_journal_status st = link->write(link->ctx, p, MM_GATT_HEADER);
        if (st != MM_JOURNAL_OK) return st;
        size_t n = 0;
        st = link->read(link->ctx, p, link->value_cap, &n);
        if (st != MM_JOURNAL_OK) return st;
        if (n <= MM_GATT_HEADER || n > link->value_cap || p[0] != REPLY ||
            mm_get32(p + 1) != at) return MM_JOURNAL_INVALID;
        size_t size = mm_get32(p + 5);
        if (size > cap) return MM_JOURNAL_TOO_SMALL;
        if ((at && total != size) || at > size || n - MM_GATT_HEADER > size - at)
            return MM_JOURNAL_INVALID;
        total = size;
        n -= MM_GATT_HEADER;
        memcpy(reply + at, p + MM_GATT_HEADER, n);
        at += n;
    } while (at < total);
    *used = total;
    return MM_JOURNAL_OK;
}
