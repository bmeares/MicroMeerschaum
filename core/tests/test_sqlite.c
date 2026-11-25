// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "micromeerschaum/sqlite.h"

static char path[256];
static const char key[] = "sensor\x1f" "reading\x1f";
static const mm_journal_config config = {
    {3, 7, key, sizeof key-1}, MM_JOURNAL_ORIGIN, MM_DESTINATION, 20, 20, 0
};
static const mm_sqlite_column columns[] = {
    {"tick", MM_VALUE_INT, false}, {"sensor_id", MM_VALUE_TEXT, true}, {"value", MM_VALUE_INT, false}
};
static const mm_sqlite_definition definition = {columns, 3, 0, false};

static mm_sqlite *store;
static mm_pipe pipe_handle;
static uint8_t scratch[2048];
static mm_journal_status open_pipe(const mm_sqlite_definition *d)
{
    mm_journal_status st = mm_sqlite_open(&store, path, &config, d);
    return st == MM_JOURNAL_OK ? mm_pipe_open(&pipe_handle, "sensor", "reading", NULL,
        mm_sqlite_target(store), scratch, sizeof scratch) : st;
}

static mm_sync_result write_value(int64_t tick, const char *id, int64_t value)
{
    mm_field fields[] = {
        {{"tick",4},{MM_VALUE_INT,{.integer=tick}}},
        {{"sensor_id",9},{MM_VALUE_NULL,{0}}},
        {{"value",5},{MM_VALUE_INT,{.integer=value}}}
    };
    if (id) fields[1].value = (mm_value){MM_VALUE_TEXT,{.text={id,strlen(id)}}};
    return mm_pipe_sync_rows(&pipe_handle, &(mm_row){fields,3}, 1, 123, 456);
}

static void reference_rows(void)
{
    assert(open_pipe(NULL) == MM_JOURNAL_OK);
    assert(write_value(1, "NA", 11).confirmed == 1);
    assert(write_value(2, "east", 22).confirmed == 1);
    assert(write_value(10000, "east", 33).confirmed == 1);
    assert(write_value(1, "NA", 44).confirmed == 1);
    assert(write_value(3, NULL, 1).confirmed == 1);
    assert(write_value(3, NULL, 2).confirmed == 1);
    assert(write_value(3, "None", 3).confirmed == 1);
    mm_sqlite_close(store);
    assert(open_pipe(NULL) == MM_JOURNAL_OK);
    mm_sqlite_query q = {.limit=9};
    mm_value latest;
    assert(mm_sqlite_get_sync_time(store, &q, &latest) == MM_JOURNAL_OK && latest.as.integer == 10000);
    mm_value begin = {MM_VALUE_INT,{.integer=1}}, end = {MM_VALUE_INT,{.integer=2}};
    mm_value ids[] = {{MM_VALUE_TEXT,{.text={"NA",2}}}, {MM_VALUE_TEXT,{.text={"east",4}}}};
    mm_sqlite_filter f = {"sensor_id",ids,1};
    q.filters = &f;
    q.filter_count = 1;
    assert(mm_sqlite_get_sync_time(store,&q,&latest) == MM_JOURNAL_OK && latest.as.integer == 1);
    q.begin = &begin;
    q.end = &end;
    f.count = 2;
    mm_sqlite_cursor *c;
    mm_row r;
    assert(mm_sqlite_get_docs(store,&q,&c) == MM_JOURNAL_OK);
    assert(mm_sqlite_next(c,&r) == MM_JOURNAL_OK && r.count == 3);
    assert(r.fields[2].value.as.integer == 44);
    assert(mm_sqlite_next(c,&r) == MM_JOURNAL_NOT_FOUND);
    mm_sqlite_cursor_close(c);
    begin.as.integer=3;
    end.as.integer=4;
    q.filter_count=0;
    assert(mm_sqlite_get_docs(store,&q,&c) == MM_JOURNAL_OK);
    assert(mm_sqlite_next(c,&r) == MM_JOURNAL_OK && r.fields[1].value.type == MM_VALUE_NULL && r.fields[2].value.as.integer == 2);
    assert(mm_sqlite_next(c,&r) == MM_JOURNAL_OK && r.fields[2].value.as.integer == 3);
    assert(mm_sqlite_next(c,&r) == MM_JOURNAL_NOT_FOUND);
    mm_sqlite_cursor_close(c);
    mm_sqlite_close(store);
}

static void retry_and_confirm(void)
{
    uint8_t bytes[1024], retry[1024];
    size_t used, again, hlen;
    uint64_t first,last,high,confirmed;
    assert(open_pipe(NULL)==MM_JOURNAL_OK);
    assert(mm_sqlite_batch(store,bytes,sizeof bytes,&used)==MM_JOURNAL_OK);
    mm_v1_batch batch;
    assert(mm_v1_batch_validate(bytes,used,&batch,&hlen,&first,&last)==MM_OK && first==1 && last==7);
    mm_record rec;
    size_t n;
    assert(mm_record_decode(bytes+hlen,used-hlen,&rec,&n)==MM_OK && rec.seq==1);
    mm_field fields[3];
    assert(mm_document_decode(rec.payload,rec.len,fields,3,&n)==MM_OK && fields[2].value.as.integer==11);
    mm_sqlite_close(store);
    assert(open_pipe(NULL)==MM_JOURNAL_OK);
    assert(mm_sqlite_batch(store,retry,sizeof retry,&again)==MM_JOURNAL_OK);
    assert(again==used && !memcmp(bytes,retry,used));
    mm_receipt receipt={config.stream,MM_DESTINATION,20,20,7};
    assert(mm_sqlite_confirm(store,21,&receipt)!=MM_JOURNAL_OK);
    assert(mm_sqlite_confirm(store,20,&receipt)==MM_JOURNAL_OK);
    mm_sqlite_close(store);
    assert(open_pipe(NULL)==MM_JOURNAL_OK);
    assert(mm_sqlite_positions(store,&high,&confirmed)==MM_JOURNAL_OK && high==7 && confirmed==7);
    assert(mm_sqlite_confirm(store,20,&receipt)==MM_JOURNAL_OK);
    assert(mm_sqlite_read(store,1,retry,sizeof retry,&again)==MM_JOURNAL_NOT_FOUND);
    assert(mm_sqlite_batch(store,retry,sizeof retry,&again)==MM_JOURNAL_NOT_FOUND);
    assert(write_value(1,"NA",45).confirmed==1);
    assert(mm_sqlite_batch(store,retry,sizeof retry,&again)==MM_JOURNAL_OK);
    assert(mm_v1_batch_validate(retry,again,&batch,&hlen,&first,&last)==MM_OK && first==8 && last==8);
    mm_sqlite_close(store);
}

int main(void)
{
    char dir[] = "/tmp/mm-sqlite-XXXXXX";
    assert(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/device.db", dir);
    mm_sqlite *db;
    assert(mm_sqlite_open(&db, path, &config, NULL) == MM_JOURNAL_NOT_FOUND);
    assert(mm_sqlite_open(&db, path, &config, &definition) == MM_JOURNAL_OK);
    mm_sqlite_close(db);
    assert(mm_sqlite_open(&db, path, &config, NULL) == MM_JOURNAL_OK);
    const mm_sqlite_definition *d = mm_sqlite_definition_get(db);
    assert(d->count == 3 && d->axis == 0 && d->columns[1].key);
    assert(!strcmp(d->columns[1].name, "sensor_id") && d->columns[1].type == MM_VALUE_TEXT);
    mm_sqlite_close(db);
    reference_rows();
    retry_and_confirm();
    unlink(path);
    rmdir(dir);
}
