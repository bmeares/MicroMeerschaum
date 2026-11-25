// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <sys/stat.h>
#include "internal.h"
#include "../src/utf8.h"

mm_journal_status mm_sql_status(int rc)
{
    switch (rc & 255) {
    case SQLITE_OK: case SQLITE_DONE: case SQLITE_ROW: return MM_JOURNAL_OK;
    case SQLITE_FULL: case SQLITE_TOOBIG: return MM_JOURNAL_FULL;
    case SQLITE_LOCKED: case SQLITE_BUSY: return MM_JOURNAL_BUSY;
    case SQLITE_NOMEM: return MM_JOURNAL_NOMEM;
    case SQLITE_NOTADB: case SQLITE_CORRUPT: return MM_JOURNAL_CORRUPT;
    case SQLITE_CONSTRAINT: return MM_JOURNAL_CONFLICT;
    default: return MM_JOURNAL_IO;
    }
}

int mm_sql_prepare(mm_sqlite *db, const char *sql, sqlite3_stmt **stmt)
{
    return sqlite3_prepare_v2(db->conn, sql, -1, stmt, NULL);
}

int mm_sql_prepare_built(mm_sqlite *db, sqlite3_str *sql, sqlite3_stmt **stmt)
{
    char *text = sqlite3_str_finish(sql);
    if (!text) return SQLITE_NOMEM;
    int rc = mm_sql_prepare(db, text, stmt);
    sqlite3_free(text);
    return rc;
}

mm_journal_status mm_sql_begin(mm_sqlite *db)
{
    if (!db) return MM_JOURNAL_INVALID;
    if (db->uncertain) return MM_JOURNAL_UNCERTAIN;
    if (db->cursors) return MM_JOURNAL_BUSY;
    return mm_sql_status(sqlite3_exec(db->conn, "BEGIN IMMEDIATE", NULL, NULL, NULL));
}

mm_journal_status mm_sql_finish(mm_sqlite *db, mm_journal_status st)
{
    if (st == MM_JOURNAL_OK) {
        int rc = sqlite3_exec(db->conn, "COMMIT", NULL, NULL, NULL);
        if (rc == SQLITE_OK) return st;
        st = mm_sql_status(rc);
        if (st == MM_JOURNAL_IO) db->uncertain = true;
    }
    if (!sqlite3_get_autocommit(db->conn) &&
        sqlite3_exec(db->conn, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK)
        db->uncertain = true;
    return db->uncertain ? MM_JOURNAL_UNCERTAIN : st;
}

int mm_sql_column(mm_sqlite *db, const char *name, size_t len)
{
    for (size_t i = 0; i < db->def.count; ++i)
        if (strlen(db->names[i]) == len && !memcmp(name, db->names[i], len)) return (int)i;
    return -1;
}

static bool valid_definition(const mm_sqlite_definition *d)
{
    if (!d || !d->count || d->count > MM_SQLITE_MAX_COLUMNS || !d->columns || d->axis >= d->count)
        return false;
    mm_value_type axis = d->columns[d->axis].type;
    if ((axis != MM_VALUE_INT && axis != MM_VALUE_TIME) || (d->autotime && axis != MM_VALUE_TIME))
        return false;
    for (size_t i = 0; i < d->count; ++i) {
        const mm_sqlite_column *col = d->columns + i;
        if (!col->name || !*col->name || strlen(col->name) > MM_SQLITE_MAX_NAMESIZE ||
            !mm_utf8_valid((const uint8_t *)col->name, strlen(col->name)) ||
            col->type <= MM_VALUE_NULL || col->type > MM_VALUE_TIME) return false;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(d->columns[j].name, col->name)) return false;
    }
    return true;
}

static int provision(mm_sqlite *db, const mm_sqlite_definition *d, const void *identity, int len)
{
    sqlite3_stmt *s = NULL;
    int rc = sqlite3_exec(db->conn,
        "CREATE TABLE mm_meta(identity BLOB NOT NULL,axis INTEGER,autotime INTEGER,"
        "high INTEGER NOT NULL,confirmed INTEGER NOT NULL);"
        "CREATE TABLE mm_columns(name TEXT NOT NULL,dtype INTEGER,is_key INTEGER);"
        "CREATE TABLE mm_out(seq INTEGER PRIMARY KEY,record BLOB NOT NULL);",
        NULL, NULL, NULL);
    if (rc == SQLITE_OK) rc = mm_sql_prepare(db, "INSERT INTO mm_meta VALUES(?,?,?,0,0)", &s);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(s, 1, identity, len, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(s, 2, (int)d->axis);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(s, 3, d->autotime);
    if (rc == SQLITE_OK) rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return rc;
    rc = mm_sql_prepare(db, "INSERT INTO mm_columns VALUES(?,?,?)", &s);
    for (size_t i = 0; i < d->count && rc == SQLITE_OK; ++i) {
        const mm_sqlite_column *col = d->columns + i;
        rc = sqlite3_bind_text(s, 1, col->name, -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(s, 2, (int)col->type);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(s, 3, col->key);
        if (rc == SQLITE_OK) rc = sqlite3_step(s);
        if (rc == SQLITE_DONE) rc = sqlite3_reset(s);
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_OK) return rc;
    sqlite3_str *sql = sqlite3_str_new(db->conn);
    sqlite3_str_appendall(sql, "CREATE TABLE mm_rows(k BLOB NOT NULL UNIQUE");
    for (size_t i = 0; i < d->count; ++i) sqlite3_str_appendf(sql, ",c%d", (int)i);
    sqlite3_str_appendf(sql, ");CREATE INDEX mm_axis ON mm_rows(c%d);", (int)d->axis);
    char *ddl = sqlite3_str_finish(sql);
    rc = ddl ? sqlite3_exec(db->conn, ddl, NULL, NULL, NULL) : SQLITE_NOMEM;
    sqlite3_free(ddl);
    return rc;
}

static mm_journal_status read_registration(mm_sqlite *db, const void *identity, int len)
{
    sqlite3_stmt *s = NULL;
    int rc = mm_sql_prepare(db, "SELECT identity,axis,autotime FROM mm_meta WHERE rowid=1", &s);
    if (rc == SQLITE_OK) rc = sqlite3_step(s);
    mm_journal_status st = mm_sql_status(rc);
    if (rc == SQLITE_DONE) st = MM_JOURNAL_CORRUPT;
    if (rc == SQLITE_ROW) {
        const void *saved = sqlite3_column_blob(s, 0);
        if (!saved || sqlite3_column_bytes(s, 0) != len || memcmp(saved, identity, (size_t)len))
            st = MM_JOURNAL_CONFLICT;
        db->def.axis = (size_t)sqlite3_column_int(s, 1);
        db->def.autotime = sqlite3_column_int(s, 2) != 0;
    }
    sqlite3_finalize(s);
    if (st != MM_JOURNAL_OK) return st;
    rc = mm_sql_prepare(db, "SELECT name,dtype,is_key FROM mm_columns ORDER BY rowid", &s);
    if (rc != SQLITE_OK) return mm_sql_status(rc);
    db->def.columns = db->columns;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        size_t i = db->def.count;
        const unsigned char *name = sqlite3_column_text(s, 0);
        int n = sqlite3_column_bytes(s, 0);
        if (i >= MM_SQLITE_MAX_COLUMNS || !name || n < 1 || (size_t)n > MM_SQLITE_MAX_NAMESIZE ||
            memchr(name, 0, (size_t)n)) {
            st = MM_JOURNAL_CORRUPT;
            break;
        }
        memcpy(db->names[i], name, (size_t)n);
        db->columns[i] = (mm_sqlite_column){db->names[i], (mm_value_type)sqlite3_column_int(s, 1),
                                            sqlite3_column_int(s, 2) != 0};
        ++db->def.count;
    }
    sqlite3_finalize(s);
    if (st != MM_JOURNAL_OK) return st;
    if (rc != SQLITE_DONE) return mm_sql_status(rc);
    return valid_definition(&db->def) ? MM_JOURNAL_OK : MM_JOURNAL_CORRUPT;
}

static bool matches(const mm_sqlite_definition *a, const mm_sqlite_definition *b)
{
    if (a->axis != b->axis || a->count != b->count || a->autotime != b->autotime) return false;
    for (size_t i = 0; i < a->count; ++i) {
        mm_sqlite_column x = a->columns[i], y = b->columns[i];
        if (strcmp(x.name, y.name) || x.key != y.key || x.type != y.type) return false;
    }
    return true;
}

mm_journal_status mm_sqlite_open(mm_sqlite **out, const char *path, const mm_journal_config *cfg,
                                 const mm_sqlite_definition *d)
{
    if (!out) return MM_JOURNAL_INVALID;
    *out = NULL;
    if (!path || !*path || !cfg || cfg->mode != MM_JOURNAL_ORIGIN || cfg->max_bytes ||
        (d && !valid_definition(d))) return MM_JOURNAL_INVALID;
    mm_receipt policy = {cfg->stream, cfg->role, cfg->issuer, cfg->destination, 0};
    uint8_t identity[MM_RECEIPT_MAX];
    int len = (int)mm_receipt_encode(identity, sizeof identity, &policy);
    if (!len) return MM_JOURNAL_INVALID;
    if (!d) {
        struct stat file;
        if (stat(path, &file) < 0) return errno == ENOENT ? MM_JOURNAL_NOT_FOUND : MM_JOURNAL_IO;
    }
    mm_sqlite *db = calloc(1, sizeof *db);
    if (!db) return MM_JOURNAL_NOMEM;
    db->config = *cfg;
    memcpy(db->key, cfg->stream.key, cfg->stream.key_len);
    db->config.stream.key = db->key;
    int flags = SQLITE_OPEN_READWRITE | (d ? SQLITE_OPEN_CREATE : 0);
    int rc = sqlite3_open_v2(path, &db->conn, flags, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_exec(db->conn, "PRAGMA locking_mode=EXCLUSIVE;PRAGMA journal_mode=PERSIST;PRAGMA synchronous=FULL;",
                          NULL, NULL, NULL);
    mm_journal_status st = mm_sql_status(rc);
    if (st == MM_JOURNAL_OK) st = mm_sql_begin(db);
    if (st == MM_JOURNAL_OK) {
        sqlite3_stmt *s = NULL;
        rc = mm_sql_prepare(db, "SELECT count(*) FROM sqlite_schema", &s);
        if (rc == SQLITE_OK) rc = sqlite3_step(s);
        bool empty = rc == SQLITE_ROW && sqlite3_column_int(s, 0) == 0;
        sqlite3_finalize(s);
        st = mm_sql_status(rc);
        if (empty) st = d ? mm_sql_status(provision(db, d, identity, len)) : MM_JOURNAL_NOT_FOUND;
        if (st == MM_JOURNAL_OK) st = read_registration(db, identity, len);
        if (st == MM_JOURNAL_OK && d && !matches(&db->def, d)) st = MM_JOURNAL_CONFLICT;
        st = mm_sql_finish(db, st);
    }
    if (st != MM_JOURNAL_OK) mm_sqlite_close(db);
    else *out = db;
    return st;
}

void mm_sqlite_close(mm_sqlite *db)
{
    if (db) { sqlite3_close(db->conn); free(db); }
}

const mm_sqlite_definition *mm_sqlite_definition_get(const mm_sqlite *db)
{
    return db ? &db->def : NULL;
}

void mm_sqlite_set_clock(mm_sqlite *db, mm_sqlite_clock clock, void *context)
{
    if (db) { db->clock = clock; db->clock_ctx = context; }
}
