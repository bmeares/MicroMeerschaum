// SPDX-License-Identifier: Apache-2.0
#include <math.h>
#include "internal.h"
#include "../src/utf8.h"

int mm_sql_bind(sqlite3_stmt *s, int slot, mm_value v)
{
    switch (v.type) {
    case MM_VALUE_NULL: return sqlite3_bind_null(s, slot);
    case MM_VALUE_INT: return sqlite3_bind_int64(s, slot, v.as.integer);
    case MM_VALUE_TIME: return sqlite3_bind_int64(s, slot, v.as.timestamp_us);
    case MM_VALUE_FLOAT: return sqlite3_bind_double(s, slot, v.as.real);
    case MM_VALUE_BOOL: return sqlite3_bind_int(s, slot, v.as.boolean);
    case MM_VALUE_TEXT: return sqlite3_bind_text(s, slot, v.as.text.data ? v.as.text.data : "",
                                                (int)v.as.text.len, SQLITE_TRANSIENT);
    }
    return SQLITE_MISMATCH;
}

struct mm_sqlite_cursor {
    mm_sqlite *db;
    sqlite3_stmt *stmt;
    mm_field fields[MM_SQLITE_MAX_COLUMNS];
    bool done;
};

static bool filter_value(mm_value v, mm_value_type t)
{
    if (v.type == MM_VALUE_NULL) return true;
    if (v.type != t) return false;
    if (t == MM_VALUE_FLOAT) return isfinite(v.as.real);
    return t != MM_VALUE_TEXT || (v.as.text.len <= MM_RECORD_MAX_PAYLOAD &&
        (!v.as.text.len || v.as.text.data) &&
        mm_utf8_valid((const uint8_t *)v.as.text.data, v.as.text.len));
}

typedef enum { SELECT_DOCS, SELECT_LATEST, SELECT_COUNT } select_mode;

static mm_journal_status select_rows(mm_sqlite *db, const mm_sqlite_query *q, select_mode mode,
                                     sqlite3_stmt **s)
{
    if (!db || !q || (mode == SELECT_DOCS && q->limit > MM_SQLITE_MAX_LIMIT) ||
        q->filter_count > MM_SQLITE_MAX_COLUMNS || (q->filter_count && !q->filters)) return MM_JOURNAL_INVALID;
    if (db->uncertain) return MM_JOURNAL_UNCERTAIN;
    int axis = (int)db->def.axis;
    mm_value_type axis_type = db->columns[axis].type;
    if ((q->begin && q->begin->type != axis_type) || (q->end && q->end->type != axis_type))
        return MM_JOURNAL_INVALID;
    size_t total = 0;
    int indices[MM_SQLITE_MAX_COLUMNS];
    for (size_t i = 0; i < q->filter_count; ++i) {
        const mm_sqlite_filter *f = q->filters + i;
        if (!f->column || (f->count && !f->values) || f->count > MM_SQLITE_MAX_VALUES-total)
            return MM_JOURNAL_INVALID;
        total += f->count;
        indices[i] = mm_sql_column(db, f->column, strlen(f->column));
        if (indices[i] < 0) return MM_JOURNAL_INVALID;
        for (size_t j = 0; j < f->count; ++j)
            if (!filter_value(f->values[j], db->columns[indices[i]].type)) return MM_JOURNAL_INVALID;
    }

    sqlite3_str *sql = sqlite3_str_new(db->conn);
    sqlite3_str_appendall(sql, "SELECT ");
    if (mode == SELECT_LATEST) sqlite3_str_appendf(sql, "MAX(c%d)", axis);
    else if (mode == SELECT_COUNT) sqlite3_str_appendall(sql, "COUNT(*)");
    else for (size_t i = 0; i < db->def.count; ++i)
        sqlite3_str_appendf(sql, "%sc%d", i ? "," : "", (int)i);
    sqlite3_str_appendall(sql, " FROM mm_rows WHERE 1");
    if (q->begin) sqlite3_str_appendf(sql, " AND c%d>=?", axis);
    if (q->end) sqlite3_str_appendf(sql, " AND c%d<?", axis);
    for (size_t i = 0; i < q->filter_count; ++i) {
        size_t n = q->filters[i].count;
        if (!n) { sqlite3_str_appendall(sql, " AND 0"); continue; }
        sqlite3_str_appendall(sql, " AND (");
        for (size_t j = 0; j < n; ++j) sqlite3_str_appendf(sql, "%sc%d IS ?", j?" OR ":"", indices[i]);
        sqlite3_str_appendall(sql, ")");
    }
    if (mode == SELECT_DOCS)
        sqlite3_str_appendf(sql, q->desc ? " ORDER BY c%d DESC,rowid DESC LIMIT %d"
                                         : " ORDER BY c%d,rowid LIMIT %d",
                            axis, (int)q->limit);
    int rc = mm_sql_prepare_built(db, sql, s), slot = 1;
    if (rc == SQLITE_OK && q->begin) rc = mm_sql_bind(*s, slot++, *q->begin);
    if (rc == SQLITE_OK && q->end) rc = mm_sql_bind(*s, slot++, *q->end);
    for (size_t i = 0; rc == SQLITE_OK && i < q->filter_count; ++i)
        for (size_t j = 0; rc == SQLITE_OK && j < q->filters[i].count; ++j)
            rc = mm_sql_bind(*s, slot++, q->filters[i].values[j]);
    return mm_sql_status(rc);
}

static mm_journal_status read_value(sqlite3_stmt *s, int i, mm_value_type type, mm_value *out)
{
    int actual = sqlite3_column_type(s, i);
    out->type = actual == SQLITE_NULL ? MM_VALUE_NULL : type;
    switch (out->type) {
    case MM_VALUE_NULL: break;
    case MM_VALUE_FLOAT:
        if (actual != SQLITE_FLOAT) return MM_JOURNAL_CORRUPT;
        out->as.real = sqlite3_column_double(s, i);
        break;
    case MM_VALUE_TEXT: {
        if (actual != SQLITE_TEXT) return MM_JOURNAL_CORRUPT;
        const char *p = (const char *)sqlite3_column_text(s, i);
        if (!p) return MM_JOURNAL_NOMEM;
        out->as.text = (mm_text){p, (size_t)sqlite3_column_bytes(s, i)};
        break;
    }
    case MM_VALUE_INT: case MM_VALUE_BOOL: case MM_VALUE_TIME: {
        if (actual != SQLITE_INTEGER) return MM_JOURNAL_CORRUPT;
        int64_t x = sqlite3_column_int64(s, i);
        if (type == MM_VALUE_BOOL) out->as.boolean = x != 0;
        else if (type == MM_VALUE_TIME) out->as.timestamp_us = x;
        else out->as.integer = x;
        break;
    }
    }
    return MM_JOURNAL_OK;
}

mm_journal_status mm_sqlite_get_docs(mm_sqlite *db, const mm_sqlite_query *q, mm_sqlite_cursor **out)
{
    if (!out) return MM_JOURNAL_INVALID;
    *out = NULL;
    sqlite3_stmt *s = NULL;
    mm_journal_status st = select_rows(db, q, SELECT_DOCS, &s);
    if (st != MM_JOURNAL_OK) { sqlite3_finalize(s); return st; }
    mm_sqlite_cursor *c = calloc(1, sizeof *c);
    if (!c) { sqlite3_finalize(s); return MM_JOURNAL_NOMEM; }
    c->stmt = s;
    c->db = db;
    ++db->cursors;
    *out = c;
    return MM_JOURNAL_OK;
}

mm_journal_status mm_sqlite_next(mm_sqlite_cursor *c, mm_row *out)
{
    if (!c || !out) return MM_JOURNAL_INVALID;
    *out = (mm_row){0};
    if (c->done) return MM_JOURNAL_NOT_FOUND;
    int rc = sqlite3_step(c->stmt);
    if (rc != SQLITE_ROW) {
        c->done = true;
        return rc == SQLITE_DONE ? MM_JOURNAL_NOT_FOUND : mm_sql_status(rc);
    }
    for (size_t i = 0; i < c->db->def.count; ++i) {
        const char *name = c->db->names[i];
        c->fields[i].name = (mm_text){name, strlen(name)};
        mm_journal_status st = read_value(c->stmt, (int)i, c->db->columns[i].type, &c->fields[i].value);
        if (st != MM_JOURNAL_OK) { c->done = true; return st; }
    }
    *out = (mm_row){c->fields, c->db->def.count};
    return MM_JOURNAL_OK;
}

void mm_sqlite_cursor_close(mm_sqlite_cursor *c)
{
    if (c) { sqlite3_finalize(c->stmt); --c->db->cursors; free(c); }
}

mm_journal_status mm_sqlite_get_sync_time(mm_sqlite *db, const mm_sqlite_query *q, mm_value *out)
{
    if (!out) return MM_JOURNAL_INVALID;
    *out = (mm_value){MM_VALUE_NULL, {0}};
    sqlite3_stmt *s = NULL;
    mm_journal_status st = select_rows(db, q, SELECT_LATEST, &s);
    if (st == MM_JOURNAL_OK) {
        int rc = sqlite3_step(s);
        st = rc == SQLITE_ROW ? read_value(s, 0, db->columns[db->def.axis].type, out) : mm_sql_status(rc);
    }
    sqlite3_finalize(s);
    return st;
}

mm_journal_status mm_sqlite_get_rowcount(mm_sqlite *db, const mm_sqlite_query *q, uint64_t *count)
{
    if (!count) return MM_JOURNAL_INVALID;
    *count = 0;
    sqlite3_stmt *s = NULL;
    mm_journal_status st = select_rows(db, q, SELECT_COUNT, &s);
    if (st == MM_JOURNAL_OK) {
        int rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) {
            if (sqlite3_column_type(s, 0) != SQLITE_INTEGER) {
                st = MM_JOURNAL_CORRUPT;
            } else {
                int64_t n = sqlite3_column_int64(s, 0);
                if (n < 0) st = MM_JOURNAL_CORRUPT;
                else *count = (uint64_t)n;
            }
        } else {
            st = rc == SQLITE_DONE ? MM_JOURNAL_CORRUPT : mm_sql_status(rc);
        }
    }
    sqlite3_finalize(s);
    return st;
}
