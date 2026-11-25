// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include <sys/time.h>
#include "py/runtime.h"
#include "../native/integers.h"
#include "../native/row.h"
#include "micromeerschaum/sqlite.h"

typedef struct {
    mp_obj_base_t base;
    mm_sqlite *db;
} store_obj;
static const mp_obj_type_t store_type;

static void check(mm_journal_status st) {
    if (st != MM_JOURNAL_OK)
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("sqlite: %s"), mm_journal_status_name(st));
}

static const char *string(mp_obj_t obj) {
    size_t len;
    if (!mp_obj_is_str(obj)) mp_raise_TypeError(MP_ERROR_TEXT("expected string"));
    const char *s = mp_obj_str_get_data(obj, &len);
    if (memchr(s, 0, len)) mp_raise_ValueError(MP_ERROR_TEXT("NUL in name or path"));
    return s;
}

static mm_sqlite *store(mp_obj_t self) {
    store_obj *obj = MP_OBJ_TO_PTR(self);
    if (!obj->db) mp_raise_ValueError(MP_ERROR_TEXT("SQLite instance closed"));
    return obj->db;
}

static mm_value_type dtype(mp_obj_t o) {
    const char *s = string(o);
    static const char *const names[] = {NULL, "bool", "int", "float", "string", "datetime"};
    for (unsigned i=1; i < MP_ARRAY_SIZE(names); ++i) if (!strcmp(s, names[i])) return (mm_value_type)i;
    mp_raise_ValueError(MP_ERROR_TEXT("unsupported SQLite dtype"));
}

static bool utc_clock(void *ctx, int64_t *out) {
    (void)ctx;
    struct timeval now;
    if (gettimeofday(&now,NULL) || now.tv_sec < 946684800) return false;
    *out = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
    return true;
}

static mp_obj_t db_open(size_t n, const mp_obj_t *a) {
    const char *path = string(a[0]), *key = string(a[1]);
    uint64_t dest = integer_bits(a[4],false);
    bool custody = n == 10 && a[9] != mp_const_none;
    mm_journal_config config = {{integer_bits(a[2],false),integer_bits(a[3],false),key,strlen(key)},
        MM_JOURNAL_ORIGIN,custody ? MM_CUSTODY : MM_DESTINATION,
        custody ? integer_bits(a[9],false) : dest,dest,0};
    mm_sqlite_column columns[MM_SQLITE_MAX_COLUMNS];
    mm_sqlite_definition def = {.columns=columns,.axis=MM_SQLITE_MAX_COLUMNS,.autotime=mp_obj_is_true(a[7])};
    bool create = mp_obj_is_true(a[8]);
    if (create) {
        mp_obj_t *items;
        mp_obj_get_array(a[5], &def.count, &items);
        if (!def.count || def.count > MM_SQLITE_MAX_COLUMNS)
            mp_raise_ValueError(MP_ERROR_TEXT("invalid column count"));
        const char *axis = string(a[6]);
        for (size_t i=0; i<def.count; ++i) {
            mp_obj_t *col;
            mp_obj_get_array_fixed_n(items[i],3,&col);
            columns[i] = (mm_sqlite_column){string(col[0]), dtype(col[1]), mp_obj_is_true(col[2])};
            if (!strcmp(columns[i].name, axis)) def.axis = i;
        }
    }
    store_obj *self = mp_obj_malloc_with_finaliser(store_obj, &store_type);
    self->db = NULL;
    check(mm_sqlite_open(&self->db,path,&config,create ? &def : NULL));
    mm_sqlite_set_clock(self->db, utc_clock, NULL);
    return MP_OBJ_FROM_PTR(self);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(open_obj, 9, 10, db_open);

static mp_obj_t db_close(mp_obj_t self) {
    store_obj *obj = MP_OBJ_TO_PTR(self);
    mm_sqlite_close(obj->db);
    obj->db = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(close_obj, db_close);

static mp_obj_t append(size_t n, const mp_obj_t *a) {
    (void)n;
    mm_pipe_target target = mm_sqlite_target(store(a[0]));
    size_t count;
    mp_obj_t *items;
    mp_obj_get_array(a[1], &count, &items);
    mm_sample group[8];
    if (!count || count > MP_ARRAY_SIZE(group)) mp_raise_ValueError(MP_ERROR_TEXT("expected 1 to 8 documents"));
    uint64_t mono = integer_bits(a[2], false);
    uint32_t boot = bounded(a[3], UINT32_MAX);
    for (size_t i=0; i<count; ++i) {
        mp_buffer_info_t b;
        mp_get_buffer_raise(items[i], &b, MP_BUFFER_READ);
        if (b.len > MM_RECORD_MAX_PAYLOAD) mp_raise_ValueError(MP_ERROR_TEXT("record too large"));
        group[i] = (mm_sample){MM_KIND_DATA, mono, boot, b.buf, (uint32_t)b.len};
    }
    mp_obj_t success_items[] = {MP_OBJ_NEW_QSTR(MP_QSTR_ok), MP_OBJ_NEW_SMALL_INT(count),
        MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(0)};
    mp_obj_t success = mp_obj_new_tuple(4, success_items);
    uint64_t first;
    mm_journal_status st = target.append(target.context,group,count,&first);
    if (st == MM_JOURNAL_OK) return success;
    bool uncertain = st == MM_JOURNAL_UNCERTAIN && first;
    const char *name = mm_journal_status_name(st);
    mp_obj_t result[] = {mp_obj_new_str(name,strlen(name)),MP_OBJ_NEW_SMALL_INT(0),
        mp_obj_new_int_from_ull(uncertain ? first : 0), mp_obj_new_int(uncertain ? count : 0)};
    return mp_obj_new_tuple(4,result);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(append_obj,4,4,append);

static mp_obj_t progress(mp_obj_t self) {
    uint64_t high, confirmed;
    check(mm_sqlite_positions(store(self),&high,&confirmed));
    mp_obj_t h = mp_obj_new_int_from_ull(high);
    mp_obj_t values[] = {h,h,mp_obj_new_int_from_ull(confirmed),MP_OBJ_NEW_SMALL_INT(0)};
    return mp_obj_new_tuple(4,values);
}
static MP_DEFINE_CONST_FUN_OBJ_1(progress_obj,progress);

static mp_obj_t batch(size_t n, const mp_obj_t *a) {
    (void)n;
    mm_sqlite *db = store(a[0]);
    uint64_t after = integer_bits(a[1],false);
    uint32_t count = bounded(a[2],UINT32_MAX);
    uint32_t cap = bounded(a[3], MM_JOURNAL_SEGMENT_MAX + MM_V1_BATCH_HEADER_MAX);
    size_t len;
    vstr_t b;
    vstr_init_len(&b,cap);
    check(mm_sqlite_batch_after(db,after,count,(uint8_t *)b.buf,cap,&len));
    b.len = len;
    return mp_obj_new_bytes_from_vstr(&b);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(batch_obj,4,4,batch);

static mp_obj_t read_record(mp_obj_t self, mp_obj_t seq_obj) {
    mm_sqlite *db = store(self);
    uint64_t seq = integer_bits(seq_obj,false);
    size_t len;
    mm_journal_status st = mm_sqlite_read(db,seq,NULL,0,&len);
    if (st != MM_JOURNAL_TOO_SMALL) check(st);
    vstr_t b;
    vstr_init_len(&b,len);
    check(mm_sqlite_read(db,seq,(uint8_t *)b.buf,len,&len));
    return mp_obj_new_bytes_from_vstr(&b);
}
static MP_DEFINE_CONST_FUN_OBJ_2(read_obj,read_record);

static mp_obj_t confirm(mp_obj_t self, mp_obj_t issuer, mp_obj_t bytes) {
    mp_buffer_info_t b;
    mp_get_buffer_raise(bytes,&b,MP_BUFFER_READ);
    mm_receipt r;
    if (mm_receipt_decode(b.buf,b.len,&r) != MM_OK) mp_raise_ValueError(MP_ERROR_TEXT("invalid receipt"));
    check(mm_sqlite_confirm(store(self),integer_bits(issuer,false),&r));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(confirm_obj,confirm);

static mm_value value_of(mp_obj_t o, mm_value_type t) {
    mm_value v = {.type=t};
    if (o == mp_const_none) { v.type=MM_VALUE_NULL; return v; }
    switch (t) {
    case MM_VALUE_INT: v.as.integer = (int64_t)integer_bits(o,true); break;
    case MM_VALUE_TIME:
        if (!mp_obj_is_int(o)) o = mp_load_attr(o, MP_QSTR_us);
        v.as.timestamp_us = (int64_t)integer_bits(o,true);
        break;
    case MM_VALUE_BOOL:
        if (!mp_obj_is_bool(o)) mp_raise_TypeError(MP_ERROR_TEXT("expected bool"));
        v.as.boolean = o == mp_const_true;
        break;
    case MM_VALUE_FLOAT: v.as.real = mp_obj_get_float(o); break;
    case MM_VALUE_TEXT:
        if (!mp_obj_is_str(o)) mp_raise_TypeError(MP_ERROR_TEXT("expected string"));
        v.as.text.data = mp_obj_str_get_data(o,&v.as.text.len);
        break;
    default: mp_raise_ValueError(MP_ERROR_TEXT("unknown dtype"));
    }
    return v;
}

typedef struct {
    mm_sqlite_query q;
    mm_sqlite_filter filters[MM_SQLITE_MAX_COLUMNS];
    mm_value values[MM_SQLITE_MAX_VALUES],begin,end;
} query_state;

static query_state *query(mm_sqlite *db, mp_obj_t begin, mp_obj_t end, mp_obj_t params) {
    query_state *state = m_new0(query_state,1);
    const mm_sqlite_definition *d = mm_sqlite_definition_get(db);
    mm_value_type axis = d->columns[d->axis].type;
    if (begin != mp_const_none) { state->begin = value_of(begin,axis); state->q.begin=&state->begin; }
    if (end != mp_const_none) { state->end = value_of(end,axis); state->q.end=&state->end; }
    if (params == mp_const_none) return state;
    if (!mp_obj_is_dict_or_ordereddict(params)) mp_raise_TypeError(MP_ERROR_TEXT("expected params dictionary"));
    mp_map_t *map = mp_obj_dict_get_map(params);
    if (map->used > MM_SQLITE_MAX_COLUMNS) mp_raise_ValueError(MP_ERROR_TEXT("too many filters"));
    state->q.filters = state->filters;
    size_t total=0;
    for (size_t i=0; i < map->alloc; ++i) {
        if (!mp_map_slot_is_filled(map,i)) continue;
        mp_obj_t o = map->table[i].value, *items = &o;
        size_t count = 1;
        if (mp_obj_is_type(o,&mp_type_list) || mp_obj_is_type(o,&mp_type_tuple))
            mp_obj_get_array(o,&count,&items);
        if (count > MM_SQLITE_MAX_VALUES-total) mp_raise_ValueError(MP_ERROR_TEXT("too many filter values"));
        const char *name = string(map->table[i].key);
        size_t col=0;
        while (col < d->count && strcmp(name,d->columns[col].name)) ++col;
        if (col == d->count) mp_raise_ValueError(MP_ERROR_TEXT("unknown filter column"));
        state->filters[state->q.filter_count++] = (mm_sqlite_filter){name,state->values+total,count};
        for (size_t j=0; j<count; ++j) state->values[total++] = value_of(items[j],d->columns[col].type);
    }
    return state;
}

typedef struct {
    nlr_jump_callback_node_t node;
    mm_sqlite_cursor *cursor;
} cursor_cleanup;

static void close_cursor(void *ctx) {
    mm_sqlite_cursor_close(((cursor_cleanup *)ctx)->cursor);
}

static mp_obj_t docs(size_t n, const mp_obj_t *a) {
    size_t budget = n == 6 ? bounded(a[5], UINT32_MAX) : SIZE_MAX;
    mm_sqlite *db=store(a[0]);
    query_state *q=query(db,a[1],a[2],a[3]);
    q->q.limit=bounded(a[4],MM_SQLITE_MAX_LIMIT);
    mp_obj_t rows=mp_obj_new_list(0,NULL);
    cursor_cleanup ctx;
    check(mm_sqlite_get_docs(db,&q->q,&ctx.cursor));
    nlr_push_jump_callback(&ctx.node,close_cursor);
    mm_row row;
    mm_journal_status st;
    while ((st=mm_sqlite_next(ctx.cursor,&row)) == MM_JOURNAL_OK) {
        /* Bound heap use before copying text from the cursor into Python.
         * The service also checks the exact encoded JSON frame length. */
        size_t bytes = 2;
        for (size_t i = 0; i < row.count; ++i) {
            bytes += row.fields[i].name.len + 4;
            if (row.fields[i].value.type == MM_VALUE_TEXT)
                bytes += row.fields[i].value.as.text.len;
        }
        if (bytes > budget) mp_raise_ValueError(MP_ERROR_TEXT("rows exceed read budget"));
        budget -= bytes;
        mp_obj_list_append(rows, mm_python_dict(row));
    }
    nlr_pop_jump_callback(true);
    if (st != MM_JOURNAL_NOT_FOUND) check(st);
    return rows;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(docs_obj,5,6,docs);

static mp_obj_t sync_time(size_t n, const mp_obj_t *a) {
    mm_sqlite *db=store(a[0]);
    query_state *q=query(db,mp_const_none,mp_const_none,n==1?mp_const_none:a[1]);
    mm_value latest;
    check(mm_sqlite_get_sync_time(db,&q->q,&latest));
    if (latest.type == MM_VALUE_NULL) return mp_const_none;
    return mp_obj_new_int_from_ll(latest.type == MM_VALUE_TIME ? latest.as.timestamp_us : latest.as.integer);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(time_obj,1,2,sync_time);

static const mp_rom_map_elem_t methods[] = {
    {MP_ROM_QSTR(MP_QSTR_close),MP_ROM_PTR(&close_obj)},
    {MP_ROM_QSTR(MP_QSTR___del__),MP_ROM_PTR(&close_obj)},
    {MP_ROM_QSTR(MP_QSTR_append),MP_ROM_PTR(&append_obj)},
    {MP_ROM_QSTR(MP_QSTR_progress),MP_ROM_PTR(&progress_obj)},
    {MP_ROM_QSTR(MP_QSTR_batch),MP_ROM_PTR(&batch_obj)},
    {MP_ROM_QSTR(MP_QSTR_confirm),MP_ROM_PTR(&confirm_obj)},
    {MP_ROM_QSTR(MP_QSTR_read_record),MP_ROM_PTR(&read_obj)},
    {MP_ROM_QSTR(MP_QSTR_docs),MP_ROM_PTR(&docs_obj)},
    {MP_ROM_QSTR(MP_QSTR_sync_time),MP_ROM_PTR(&time_obj)}
};
static MP_DEFINE_CONST_DICT(methods_dict,methods);
static MP_DEFINE_CONST_OBJ_TYPE(store_type,MP_QSTR_SQLite,MP_TYPE_FLAG_NONE,locals_dict,&methods_dict);
static const mp_rom_map_elem_t globals[] = {
    {MP_ROM_QSTR(MP_QSTR___name__),MP_ROM_QSTR(MP_QSTR__mmsqlite)},
    {MP_ROM_QSTR(MP_QSTR_open),MP_ROM_PTR(&open_obj)}
};
static MP_DEFINE_CONST_DICT(globals_dict,globals);
const mp_obj_module_t mmsqlite_module = {.base={&mp_type_module},.globals=(mp_obj_dict_t *)&globals_dict};
MP_REGISTER_MODULE(MP_QSTR__mmsqlite,mmsqlite_module);
