// SPDX-License-Identifier: Apache-2.0
#include "internal.h"

typedef struct {
    mm_field fields[MM_SQLITE_MAX_COLUMNS];
    mm_field key[MM_SQLITE_MAX_COLUMNS];
    int column[MM_SQLITE_MAX_COLUMNS];
    size_t count, keys;
} write_row;

static mm_journal_status prepare_row(mm_sqlite *db, const mm_sample *sample, write_row *row)
{
    if (sample->kind != MM_KIND_DATA || mm_document_decode(sample->payload, sample->len,
            row->fields, db->def.count, &row->count) != MM_OK) return MM_JOURNAL_INVALID;
    int positions[MM_SQLITE_MAX_COLUMNS];
    for (size_t i = 0; i < db->def.count; ++i) positions[i] = -1;
    for (size_t i = 0; i < row->count; ++i) {
        mm_field f = row->fields[i];
        int col = mm_sql_column(db, f.name.data, f.name.len);
        if (col < 0 || (f.value.type != MM_VALUE_NULL && f.value.type != db->columns[col].type))
            return MM_JOURNAL_INVALID;
        row->column[i] = col;
        positions[col] = (int)i;
    }
    size_t axis = db->def.axis;
    if (positions[axis] < 0 && db->def.autotime) {
        int64_t utc;
        if (!db->clock || !db->clock(db->clock_ctx, &utc)) return MM_JOURNAL_INVALID;
        size_t pos = row->count++;
        row->fields[pos] = (mm_field){{db->names[axis], strlen(db->names[axis])},
                                     {MM_VALUE_TIME, {.timestamp_us = utc}}};
        row->column[pos] = (int)axis;
        positions[axis] = (int)pos;
    }
    if (positions[axis] < 0 || row->fields[positions[axis]].value.type == MM_VALUE_NULL)
        return MM_JOURNAL_INVALID;
    row->keys = 0;
    for (size_t i = 0; i < db->def.count; ++i) {
        if (i != axis && !db->columns[i].key) continue;
        mm_field k = {{db->names[i], strlen(db->names[i])}, {MM_VALUE_NULL,{0}}};
        if (positions[i] >= 0) k.value = row->fields[positions[i]].value;
        if (k.value.type == MM_VALUE_FLOAT && k.value.as.real == 0.0) k.value.as.real = 0.0;
        row->key[row->keys++] = k;
    }
    return MM_JOURNAL_OK;
}

static uint8_t *encode(mm_row r, size_t prefix, size_t *len, mm_journal_status *st)
{
    if (mm_document_encode(NULL, 0, r, len) != MM_OK) { *st = MM_JOURNAL_INVALID; return NULL; }
    uint8_t *buf = malloc(prefix + *len);
    if (!buf) { *st = MM_JOURNAL_NOMEM; return NULL; }
    if (mm_document_encode(buf + prefix, *len, r, len) != MM_OK) {
        free(buf); *st = MM_JOURNAL_INVALID; return NULL;
    }
    return buf;
}

static mm_journal_status upsert(mm_sqlite *db, const write_row *row)
{
    size_t len;
    mm_journal_status st = MM_JOURNAL_OK;
    uint8_t *key = encode((mm_row){row->key,row->keys}, 0, &len, &st);
    if (!key) return st;
    sqlite3_str *sql = sqlite3_str_new(db->conn);
    sqlite3_str_appendall(sql, "INSERT INTO mm_rows(k");
    for (size_t i = 0; i < row->count; ++i) sqlite3_str_appendf(sql,",c%d",row->column[i]);
    sqlite3_str_appendall(sql, ") VALUES(?");
    for (size_t i = 0; i < row->count; ++i) sqlite3_str_appendall(sql,",?");
    sqlite3_str_appendall(sql, ") ON CONFLICT(k) DO UPDATE SET ");
    for (size_t i = 0; i < row->count; ++i)
        sqlite3_str_appendf(sql, "%sc%d=excluded.c%d", i?",":"", row->column[i], row->column[i]);
    sqlite3_stmt *s = NULL;
    int rc = mm_sql_prepare_built(db, sql, &s);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(s, 1, key, (int)len, SQLITE_STATIC);
    for (size_t i = 0; rc == SQLITE_OK && i < row->count; ++i)
        rc = mm_sql_bind(s, (int)i+2, row->fields[i].value);
    if (rc == SQLITE_OK) rc = sqlite3_step(s);
    sqlite3_finalize(s);
    free(key);
    return mm_sql_status(rc);
}

static mm_journal_status append_sample(mm_sqlite *db, const mm_sample *sample, uint64_t seq, write_row *row)
{
    mm_journal_status st = prepare_row(db, sample, row);
    if (st != MM_JOURNAL_OK) return st;
    st = upsert(db, row);
    if (st != MM_JOURNAL_OK) return st;
    size_t len;
    uint8_t *blob = encode((mm_row){row->fields,row->count}, MM_RECORD_HEADER_SIZE, &len, &st);
    if (!blob) return st;
    if (mm_record_header(blob, MM_KIND_DATA, seq, sample->mono_us, sample->boot,
                         blob + MM_RECORD_HEADER_SIZE, (uint32_t)len) != MM_OK) {
        free(blob); return MM_JOURNAL_INVALID;
    }
    sqlite3_stmt *s = NULL;
    int rc = mm_sql_prepare(db, "INSERT INTO mm_out(seq,record) VALUES(?,?)", &s);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 1, (int64_t)seq);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(s, 2, blob, (int)(MM_RECORD_HEADER_SIZE + len), SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(s);
    sqlite3_finalize(s);
    free(blob);
    return mm_sql_status(rc);
}

mm_journal_status mm_sqlite_positions(mm_sqlite *db, uint64_t *high, uint64_t *confirmed)
{
    if (!db || !high || !confirmed) return MM_JOURNAL_INVALID;
    if (db->uncertain) return MM_JOURNAL_UNCERTAIN;
    sqlite3_stmt *s = NULL;
    int rc = mm_sql_prepare(db, "SELECT high,confirmed FROM mm_meta WHERE rowid=1", &s);
    if (rc == SQLITE_OK) rc = sqlite3_step(s);
    mm_journal_status st = mm_sql_status(rc);
    if (rc == SQLITE_ROW) {
        int64_t h = sqlite3_column_int64(s, 0), c = sqlite3_column_int64(s, 1);
        if (c < 0 || h < c) st = MM_JOURNAL_CORRUPT;
        else { *high = (uint64_t)h; *confirmed = (uint64_t)c; }
    } else if (rc == SQLITE_DONE) st = MM_JOURNAL_CORRUPT;
    sqlite3_finalize(s);
    return st;
}

mm_journal_status mm_sql_append(void *ctx, const mm_sample *samples, size_t count, uint64_t *first)
{
    mm_sqlite *db = ctx;
    if (!first) return MM_JOURNAL_INVALID;
    *first = 0;
    if (!samples || !count || !db) return MM_JOURNAL_INVALID;
    mm_journal_status st = mm_sql_begin(db);
    if (st != MM_JOURNAL_OK) return st;
    uint64_t high = 0, confirmed;
    st = mm_sqlite_positions(db, &high, &confirmed);
    if (st == MM_JOURNAL_OK && count > INT64_MAX-high) st = MM_JOURNAL_FULL;
    write_row *row = NULL;
    if (st == MM_JOURNAL_OK) {
        *first = high+1;
        row = malloc(sizeof *row);
        if (!row) st = MM_JOURNAL_NOMEM;
    }
    for (size_t i = 0; i < count && st == MM_JOURNAL_OK; ++i)
        st = append_sample(db, samples+i, ++high, row);
    free(row);
    if (st == MM_JOURNAL_OK) {
        sqlite3_stmt *s = NULL;
        int rc = mm_sql_prepare(db, "UPDATE mm_meta SET high=? WHERE rowid=1", &s);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 1, (int64_t)high);
        if (rc == SQLITE_OK) rc = sqlite3_step(s);
        sqlite3_finalize(s);
        st = mm_sql_status(rc);
    }
    return mm_sql_finish(db, st);
}

mm_pipe_target mm_sqlite_target(mm_sqlite *db)
{
    if (!db) return (mm_pipe_target){0};
    return (mm_pipe_target){db->config.stream, mm_sql_append, db};
}
