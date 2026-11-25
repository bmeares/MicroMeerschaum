// SPDX-License-Identifier: Apache-2.0
#ifndef MM_SQLITE_INTERNAL_H
#define MM_SQLITE_INTERNAL_H
#include "micromeerschaum/sqlite.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

struct mm_sqlite {
    sqlite3 *conn;
    mm_journal_config config;
    char key[MM_KEY_MAX];
    mm_sqlite_definition def;
    mm_sqlite_column columns[MM_SQLITE_MAX_COLUMNS];
    char names[MM_SQLITE_MAX_COLUMNS][MM_SQLITE_MAX_NAMESIZE + 1];
    mm_sqlite_clock clock;
    void *clock_ctx;
    bool uncertain;
    size_t cursors;
};

mm_journal_status mm_sql_status(int rc);
int mm_sql_prepare(mm_sqlite *db, const char *sql, sqlite3_stmt **stmt);
int mm_sql_prepare_built(mm_sqlite *db, sqlite3_str *sql, sqlite3_stmt **stmt);
mm_journal_status mm_sql_begin(mm_sqlite *db);
mm_journal_status mm_sql_finish(mm_sqlite *db, mm_journal_status status);
int mm_sql_bind(sqlite3_stmt *s, int slot, mm_value value);
int mm_sql_column(mm_sqlite *db, const char *name, size_t len);
mm_journal_status mm_sql_append(void *, const mm_sample *, size_t, uint64_t *);
#endif
