// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include "py/runtime.h"
#include "py/objint.h"
#include "micromeerschaum/document.h"
#include "micromeerschaum/peer.h"

static void check(mm_journal_status status) {
    if (status != MM_JOURNAL_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("journal: %s"),
            mm_journal_status_name(status));
    }
}

static uint64_t integer_bits(mp_obj_t value, bool is_signed) {
    if (!mp_obj_is_int(value)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected integer"));
    }
    bool negative = mp_obj_int_sign(value) < 0;
    if (!is_signed && negative) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected unsigned integer"));
    }
    if (mp_obj_is_small_int(value)) {
        return (uint64_t)MP_OBJ_SMALL_INT_VALUE(value);
    }
    byte le[8];
    if (!mp_obj_int_to_bytes_impl(value, false, sizeof le, le)
        || (is_signed && negative != ((le[7] & 128) != 0))) {
        mp_raise_ValueError(MP_ERROR_TEXT("integer outside 64-bit range"));
    }
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) {
        n = (n << 8) | le[i];
    }
    return n;
}

static uint32_t bounded(mp_obj_t value, uint32_t max) {
    uint64_t n = integer_bits(value, false);
    if (n > max) {
        mp_raise_ValueError(MP_ERROR_TEXT("value exceeds limit"));
    }
    return (uint32_t)n;
}

static mm_text text_value(mp_obj_t obj) {
    if (!mp_obj_is_str(obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected string"));
    }
    size_t len;
    const char *str = mp_obj_str_get_data(obj, &len);
    return (mm_text){str, len};
}

typedef struct {
    mp_obj_base_t base;
    int64_t us;
} timestamp_obj;

static mp_obj_t timestamp_new(const mp_obj_type_t *type, size_t n, size_t kw, const mp_obj_t *args) {
    mp_arg_check_num(n, kw, 1, 1, false);
    timestamp_obj *stamp = mp_obj_malloc(timestamp_obj, type);
    stamp->us = (int64_t)integer_bits(args[0], true);
    return MP_OBJ_FROM_PTR(stamp);
}

static void timestamp_attr(mp_obj_t self, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_us) {
        timestamp_obj *stamp = MP_OBJ_TO_PTR(self);
        dest[0] = mp_obj_new_int_from_ll(stamp->us);
    }
}

static MP_DEFINE_CONST_OBJ_TYPE(timestamp_type, MP_QSTR_Timestamp, MP_TYPE_FLAG_NONE,
    make_new, timestamp_new, attr, timestamp_attr);

static mm_value scalar(mp_obj_t v) {
    if (v == mp_const_none) {
        return (mm_value){.type = MM_VALUE_NULL};
    }
    if (mp_obj_is_bool(v)) {
        return (mm_value){.type = MM_VALUE_BOOL, .as.boolean = v == mp_const_true};
    }
    if (mp_obj_is_int(v)) {
        return (mm_value){.type = MM_VALUE_INT, .as.integer = (int64_t)integer_bits(v, true)};
    }
    if (mp_obj_is_float(v)) {
        return (mm_value){.type = MM_VALUE_FLOAT, .as.real = mp_obj_get_float(v)};
    }
    if (mp_obj_is_str(v)) {
        return (mm_value){.type = MM_VALUE_TEXT, .as.text = text_value(v)};
    }
    if (mp_obj_is_type(v, &timestamp_type)) {
        timestamp_obj *stamp = MP_OBJ_TO_PTR(v);
        return (mm_value){.type = MM_VALUE_TIME, .as.timestamp_us = stamp->us};
    }
    mp_raise_TypeError(MP_ERROR_TEXT("unsupported document value"));
}

static mp_obj_t mm_encode(mp_obj_t pairs) {
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(pairs, &n, &items);
    if (n > MM_DOCUMENT_MAX_FIELDS) {
        mp_raise_ValueError(MP_ERROR_TEXT("too many fields"));
    }
    mm_field *fields = m_new(mm_field, n);
    for (size_t i = 0; i < n; ++i) {
        mp_obj_t *pair;
        mp_obj_get_array_fixed_n(items[i], 2, &pair);
        fields[i] = (mm_field){text_value(pair[0]), scalar(pair[1])};
    }
    size_t len;
    const mm_row row = {fields, n};
    if (mm_document_encode(NULL, 0, row, &len) != MM_OK) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid document"));
    }
    vstr_t buf;
    vstr_init_len(&buf, len);
    if (mm_document_encode((uint8_t *)buf.buf, len, row, &len) != MM_OK) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid document"));
    }
    m_del(mm_field, fields, n);
    return mp_obj_new_bytes_from_vstr(&buf);
}
static MP_DEFINE_CONST_FUN_OBJ_1(encode_obj, mm_encode);

static mp_obj_t from_scalar(mm_value v) {
    switch (v.type) {
        case MM_VALUE_NULL: return mp_const_none;
        case MM_VALUE_BOOL: return mp_obj_new_bool(v.as.boolean);
        case MM_VALUE_INT: return mp_obj_new_int_from_ll(v.as.integer);
        case MM_VALUE_FLOAT: return mp_obj_new_float(v.as.real);
        case MM_VALUE_TEXT: return mp_obj_new_str(v.as.text.data, v.as.text.len);
        case MM_VALUE_TIME: {
            timestamp_obj *stamp = mp_obj_malloc(timestamp_obj, &timestamp_type);
            stamp->us = v.as.timestamp_us;
            return MP_OBJ_FROM_PTR(stamp);
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("invalid field type"));
}

static mp_obj_t mm_decode(mp_obj_t payload) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(payload, &buf, MP_BUFFER_READ);
    mm_field *fields = m_new(mm_field, MM_DOCUMENT_MAX_FIELDS);
    size_t n;
    if (mm_document_decode(buf.buf, buf.len, fields, MM_DOCUMENT_MAX_FIELDS, &n) != MM_OK) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid document"));
    }
    mp_obj_t result = mp_obj_new_dict(n);
    for (size_t i = 0; i < n; ++i) {
        mp_obj_dict_store(result, mp_obj_new_str(fields[i].name.data, fields[i].name.len),
            from_scalar(fields[i].value));
    }
    m_del(mm_field, fields, MM_DOCUMENT_MAX_FIELDS);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(decode_obj, mm_decode);

typedef struct {
    mp_obj_base_t base;
    mm_journal *journal;
} volume_obj;

static const mp_obj_type_t volume_type;

static mm_journal *store(mp_obj_t value) {
    volume_obj *self = MP_OBJ_TO_PTR(value);
    if (!self->journal) {
        mp_raise_ValueError(MP_ERROR_TEXT("volume closed"));
    }
    return self->journal;
}

static mp_obj_t open_volume(size_t n, const mp_obj_t *a) {
    (void)n;
    mm_text path = text_value(a[0]), key = text_value(a[1]);
    if (memchr(path.data, 0, path.len)) {
        mp_raise_ValueError(MP_ERROR_TEXT("NUL in volume path"));
    }
    mm_journal_config config = {
        .stream = {integer_bits(a[2], false), integer_bits(a[3], false), key.data, key.len},
        .mode = mp_obj_is_true(a[5]) ? MM_JOURNAL_RECEIVER : MM_JOURNAL_ORIGIN,
        .role = MM_DESTINATION,
        .issuer = integer_bits(a[4], false),
        .destination = integer_bits(a[4], false),
    };
    mm_volume_options options = {
        (uint16_t)bounded(a[6], UINT16_MAX), bounded(a[7], MM_JOURNAL_SEGMENT_MAX),
        mp_obj_is_true(a[8]) ? MM_KEEP_NEW : MM_KEEP_OLD,
    };
    volume_obj *self = mp_obj_malloc_with_finaliser(volume_obj, &volume_type);
    self->journal = NULL;
    check(mm_journal_open_volume(&self->journal, path.data, &config, &options, mp_obj_is_true(a[9])));
    return MP_OBJ_FROM_PTR(self);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(open_volume_obj, 10, 10, open_volume);

static mp_obj_t close_volume(mp_obj_t value) {
    volume_obj *self = MP_OBJ_TO_PTR(value);
    mm_journal_close(self->journal);
    self->journal = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(close_obj, close_volume);

static mp_obj_t progress(mp_obj_t value) {
    mm_journal *j = store(value);
    if (mm_journal_needs_reopen(j)) {
        check(MM_JOURNAL_UNCERTAIN);
    }
    mp_obj_t positions[] = {
        mp_obj_new_int_from_ull(mm_journal_prefix(j)), mp_obj_new_int_from_ull(mm_journal_high(j)),
        mp_obj_new_int_from_ull(mm_journal_confirmed(j)), mp_obj_new_int_from_ull(mm_journal_missed(j)),
    };
    return mp_obj_new_tuple(4, positions);
}
static MP_DEFINE_CONST_FUN_OBJ_1(progress_obj, progress);

static mp_obj_t append(size_t n, const mp_obj_t *a) {
    (void)n;
    mm_journal *j = store(a[0]);
    size_t count;
    mp_obj_t *items;
    mp_obj_get_array(a[1], &count, &items);
    mm_sample samples[8];
    if (!count || count > MP_ARRAY_SIZE(samples)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected 1 to 8 documents"));
    }
    uint64_t mono_us = integer_bits(a[2], false);
    uint32_t boot = bounded(a[3], UINT32_MAX);
    for (size_t i = 0; i < count; ++i) {
        mp_buffer_info_t data;
        mp_get_buffer_raise(items[i], &data, MP_BUFFER_READ);
        if (data.len > MM_RECORD_MAX_PAYLOAD) {
            mp_raise_ValueError(MP_ERROR_TEXT("record too large"));
        }
        samples[i] = (mm_sample){MM_KIND_DATA, mono_us, boot, data.buf, (uint32_t)data.len};
    }
    uint64_t first = 0;
    mm_journal_status status = mm_journal_append_group(j, samples, count, &first);
    bool uncertain = status == MM_JOURNAL_UNCERTAIN && first != 0;
    const char *name = mm_journal_status_name(status);
    mp_obj_t result[] = {mp_obj_new_str(name, strlen(name)),
        mp_obj_new_int(status == MM_JOURNAL_OK ? count : 0),
        mp_obj_new_int_from_ull(uncertain ? first : 0), mp_obj_new_int(uncertain ? count : 0)};
    return mp_obj_new_tuple(4, result);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(append_obj, 4, 4, append);

static mp_obj_t batch(size_t n, const mp_obj_t *a) {
    (void)n;
    mm_journal *j = store(a[0]);
    uint64_t after = integer_bits(a[1], false);
    uint32_t limit = bounded(a[2], UINT32_MAX);
    size_t cap = bounded(a[3], MM_JOURNAL_SEGMENT_MAX + MM_V1_BATCH_HEADER_MAX);
    vstr_t buf;
    vstr_init_len(&buf, cap);
    size_t used;
    check(mm_peer_batch(j, after, limit, (uint8_t *)buf.buf, cap, &used));
    buf.len = used;
    return mp_obj_new_bytes_from_vstr(&buf);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(batch_obj, 4, 4, batch);

static mp_obj_t receive(mp_obj_t self, mp_obj_t origin, mp_obj_t frame) {
    mp_buffer_info_t data;
    mp_get_buffer_raise(frame, &data, MP_BUFFER_READ);
    uint8_t reply[MM_RECEIPT_MAX];
    size_t used;
    check(mm_peer_receive(store(self), integer_bits(origin, false), data.buf, data.len,
        reply, sizeof reply, &used));
    return mp_obj_new_bytes(reply, used);
}
static MP_DEFINE_CONST_FUN_OBJ_3(receive_obj, receive);

static mp_obj_t confirm(mp_obj_t self, mp_obj_t issuer, mp_obj_t receipt) {
    mp_buffer_info_t data;
    mp_get_buffer_raise(receipt, &data, MP_BUFFER_READ);
    check(mm_peer_confirm(store(self), integer_bits(issuer, false), data.buf, data.len));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(confirm_obj, confirm);

static mp_obj_t read_record(mp_obj_t self, mp_obj_t seq_obj) {
    mm_journal *j = store(self);
    uint64_t seq = integer_bits(seq_obj, false);
    size_t len;
    mm_journal_status status = mm_journal_read(j, seq, NULL, 0, &len);
    if (status != MM_JOURNAL_TOO_SMALL) {
        check(status);
    }
    vstr_t buf;
    vstr_init_len(&buf, len);
    check(mm_journal_read(j, seq, (uint8_t *)buf.buf, len, &len));
    return mp_obj_new_bytes_from_vstr(&buf);
}
static MP_DEFINE_CONST_FUN_OBJ_2(read_obj, read_record);

static mp_obj_t reclaim(mp_obj_t self) {
    check(mm_journal_reclaim(store(self)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(reclaim_obj, reclaim);

static const mp_rom_map_elem_t methods[] = {
    {MP_ROM_QSTR(MP_QSTR_append), MP_ROM_PTR(&append_obj)},
    {MP_ROM_QSTR(MP_QSTR_progress), MP_ROM_PTR(&progress_obj)},
    {MP_ROM_QSTR(MP_QSTR_batch), MP_ROM_PTR(&batch_obj)},
    {MP_ROM_QSTR(MP_QSTR_receive), MP_ROM_PTR(&receive_obj)},
    {MP_ROM_QSTR(MP_QSTR_confirm), MP_ROM_PTR(&confirm_obj)},
    {MP_ROM_QSTR(MP_QSTR_read_record), MP_ROM_PTR(&read_obj)},
    {MP_ROM_QSTR(MP_QSTR_reclaim), MP_ROM_PTR(&reclaim_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&close_obj)},
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&close_obj)},
};
static MP_DEFINE_CONST_DICT(method_dict, methods);
static MP_DEFINE_CONST_OBJ_TYPE(volume_type, MP_QSTR_Volume, MP_TYPE_FLAG_NONE,
    locals_dict, &method_dict);

static const mp_rom_map_elem_t globals[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__mm)},
    {MP_ROM_QSTR(MP_QSTR_Timestamp), MP_ROM_PTR(&timestamp_type)},
    {MP_ROM_QSTR(MP_QSTR_encode), MP_ROM_PTR(&encode_obj)},
    {MP_ROM_QSTR(MP_QSTR_decode), MP_ROM_PTR(&decode_obj)},
    {MP_ROM_QSTR(MP_QSTR_open_volume), MP_ROM_PTR(&open_volume_obj)},
};
static MP_DEFINE_CONST_DICT(globals_dict, globals);
const mp_obj_module_t mm_module = {.base = {&mp_type_module}, .globals = (mp_obj_dict_t *)&globals_dict};
MP_REGISTER_MODULE(MP_QSTR__mm, mm_module);
