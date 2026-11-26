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

    /* Descending order: newest axis first, breaking ties by rowid DESC. */
    mm_sqlite_query desc_q = {.limit=9, .desc=true};
    assert(mm_sqlite_get_docs(store, &desc_q, &c) == MM_JOURNAL_OK);
    const int64_t expected_ticks[] = {10000, 3, 3, 2, 1};
    const int64_t expected_vals[]  = {33, 3, 2, 22, 44};
    for (size_t i = 0; i < 5; ++i) {
        assert(mm_sqlite_next(c, &r) == MM_JOURNAL_OK);
        assert(r.fields[0].value.as.integer == expected_ticks[i]);
        assert(r.fields[2].value.as.integer == expected_vals[i]);
    }
    assert(mm_sqlite_next(c, &r) == MM_JOURNAL_NOT_FOUND);
    mm_sqlite_cursor_close(c);

    /* Descending with bounds, filter, and limit. */
    begin.as.integer = 1;
    end.as.integer = 4;
    desc_q.begin = &begin;
    desc_q.end = &end;
    desc_q.limit = 2;
    assert(mm_sqlite_get_docs(store, &desc_q, &c) == MM_JOURNAL_OK);
    assert(mm_sqlite_next(c, &r) == MM_JOURNAL_OK && r.fields[0].value.as.integer == 3 && r.fields[2].value.as.integer == 3);
    assert(mm_sqlite_next(c, &r) == MM_JOURNAL_OK && r.fields[0].value.as.integer == 3 && r.fields[2].value.as.integer == 2);
    assert(mm_sqlite_next(c, &r) == MM_JOURNAL_NOT_FOUND);
    mm_sqlite_cursor_close(c);

    /* Rowcount: unfiltered, half-open bounds, filters, NULL filter, empty filter list, and ignored limit. */
    uint64_t count = 999;
    mm_sqlite_query rc_q = {.limit = MM_SQLITE_MAX_LIMIT + 100};
    assert(mm_sqlite_get_rowcount(store, &rc_q, &count) == MM_JOURNAL_OK && count == 5);
    rc_q.limit = 0;
    assert(mm_sqlite_get_rowcount(store, &rc_q, &count) == MM_JOURNAL_OK && count == 5);
    rc_q.begin = &begin;
    rc_q.end = &end;
    assert(mm_sqlite_get_rowcount(store, &rc_q, &count) == MM_JOURNAL_OK && count == 4);
    f.count = 2;
    rc_q.filters = &f;
    rc_q.filter_count = 1;
    assert(mm_sqlite_get_rowcount(store, &rc_q, &count) == MM_JOURNAL_OK && count == 2);
    mm_value null_val = {MM_VALUE_NULL, {0}};
    mm_sqlite_filter null_f = {"sensor_id", &null_val, 1};
    rc_q.filters = &null_f;
    assert(mm_sqlite_get_rowcount(store, &rc_q, &count) == MM_JOURNAL_OK && count == 1);
    mm_sqlite_filter empty_f = {"sensor_id", NULL, 0};
    rc_q.filters = &empty_f;
    count = 999;
    assert(mm_sqlite_get_rowcount(store, &rc_q, &count) == MM_JOURNAL_OK && count == 0);

    /* Rowcount validation errors. */
    assert(mm_sqlite_get_rowcount(NULL, &rc_q, &count) == MM_JOURNAL_INVALID);
    assert(mm_sqlite_get_rowcount(store, NULL, &count) == MM_JOURNAL_INVALID);
    assert(mm_sqlite_get_rowcount(store, &rc_q, NULL) == MM_JOURNAL_INVALID);
    mm_value bad_bound = {MM_VALUE_TEXT, {.text={"x", 1}}};
    mm_sqlite_query bad_q = {.begin = &bad_bound};
    assert(mm_sqlite_get_rowcount(store, &bad_q, &count) == MM_JOURNAL_INVALID);
    mm_sqlite_filter bad_col = {"missing", &ids[0], 1};
    bad_q = (mm_sqlite_query){.filters = &bad_col, .filter_count = 1};
    assert(mm_sqlite_get_rowcount(store, &bad_q, &count) == MM_JOURNAL_INVALID);
    mm_value bad_type = {MM_VALUE_INT, {.integer = 7}};
    mm_sqlite_filter bad_val = {"sensor_id", &bad_type, 1};
    bad_q = (mm_sqlite_query){.filters = &bad_val, .filter_count = 1};
    assert(mm_sqlite_get_rowcount(store, &bad_q, &count) == MM_JOURNAL_INVALID);

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

static bool step_clock(void *ctx, int64_t *utc)
{
    *utc = ++*(int64_t *)ctx;
    return true;
}

static void generated_axis(void)
{
    const mm_sqlite_column time_columns[] = {{"event",MM_VALUE_TIME,true},{"value",MM_VALUE_INT,false}};
    const mm_sqlite_definition def = {time_columns,2,0,true};
    assert(open_pipe(&def)==MM_JOURNAL_OK);
    mm_sqlite_query q = {.limit=2};
    mm_value latest;
    uint64_t count = 7;
    assert(mm_sqlite_get_rowcount(store,&q,&count)==MM_JOURNAL_OK && count==0);
    assert(mm_sqlite_get_sync_time(store,&q,&latest)==MM_JOURNAL_OK && latest.type==MM_VALUE_NULL);
    const mm_field field={{"value",5},{MM_VALUE_INT,{.integer=13}}};
    mm_row batch[]={{&field,1},{&field,1}};
    assert(mm_pipe_sync_rows(&pipe_handle,batch,2,12,1).status==MM_JOURNAL_INVALID);
    int64_t utc=100;
    mm_sqlite_set_clock(store,step_clock,&utc);
    assert(mm_pipe_sync_rows(&pipe_handle,batch,2,12,1).confirmed==2);
    assert(mm_sqlite_get_sync_time(store,&q,&latest)==MM_JOURNAL_OK && latest.type==MM_VALUE_TIME &&
           latest.as.timestamp_us==101);
    mm_sqlite_close(store);
    assert(open_pipe(NULL)==MM_JOURNAL_OK);
    uint8_t data[128];
    for (uint64_t seq=1;seq<=2;++seq) {
        size_t n, used;
        mm_record rec;
        mm_field fields[2];
        assert(mm_sqlite_read(store,seq,data,sizeof data,&n)==MM_JOURNAL_OK);
        assert(mm_record_decode(data,n,&rec,&used)==MM_OK && rec.seq==seq);
        assert(mm_document_decode(rec.payload,rec.len,fields,2,&n)==MM_OK && n==2 &&
               fields[1].value.type==MM_VALUE_TIME && fields[1].value.as.timestamp_us==101);
    }
    mm_field explicit_time={{"event",5},{MM_VALUE_TIME,{.timestamp_us=102}}};
    assert(mm_pipe_sync_rows(&pipe_handle,&(mm_row){&explicit_time,1},1,0,0).confirmed==1);
    assert(mm_sqlite_get_sync_time(store,&q,&latest)==MM_JOURNAL_OK && latest.as.timestamp_us==102);
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
    char sidecar[280];
    snprintf(sidecar,sizeof sidecar,"%s-journal",path);
    unlink(sidecar);
    generated_axis();
    unlink(path);
    unlink(sidecar);
    assert(!rmdir(dir));
}
