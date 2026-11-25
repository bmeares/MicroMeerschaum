// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include "py/runtime.h"
#include "micromeerschaum/https.h"

typedef struct {
    mp_obj_base_t base;
    mm_https_client *client;
    mp_obj_t ca;
} https_obj;
static const mp_obj_type_t client_type;

static const char *string_arg(mp_obj_t value) {
    size_t n;
    const char *s = mp_obj_str_get_data(value, &n);
    if (memchr(s, 0, n)) mp_raise_ValueError(MP_ERROR_TEXT("embedded NUL"));
    return s;
}
static mp_obj_t open_client(size_t n_args, const mp_obj_t *args) {
    const char *url = string_arg(args[0]), *token = string_arg(args[1]);
    mp_obj_t ca = n_args == 3 ? args[2] : mp_const_none;
    const char *pem = ca == mp_const_none ? NULL : string_arg(ca);
    https_obj *self = mp_obj_malloc_with_finaliser(https_obj, &client_type);
    self->ca = ca;  // IDF may borrow the PEM until cleanup.
    self->client = NULL;
    mm_journal_status status = mm_https_open(&self->client, url, token, pem, 15000);
    if (status != MM_JOURNAL_OK)
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("HTTPS: %s"), mm_journal_status_name(status));
    return MP_OBJ_FROM_PTR(self);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(open_obj, 2, 3, open_client);

static mp_obj_t exchange(mp_obj_t self_in, mp_obj_t batch) {
    https_obj *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t bytes;
    mp_get_buffer_raise(batch, &bytes, MP_BUFFER_READ);
    uint8_t reply[MM_RECEIPT_MAX];
    size_t used;
    mm_journal_status status = mm_https_exchange(self->client, bytes.buf, bytes.len, reply, sizeof reply, &used);
    if (status != MM_JOURNAL_OK)
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("HTTPS: %s"), mm_journal_status_name(status));
    return mp_obj_new_bytes(reply, used);
}
static MP_DEFINE_CONST_FUN_OBJ_2(exchange_obj, exchange);

static mp_obj_t close_client(mp_obj_t self_in) {
    https_obj *self = MP_OBJ_TO_PTR(self_in);
    if (self->client) mm_https_close(self->client);
    self->client = NULL;
    self->ca = mp_const_none;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(close_obj, close_client);

static const mp_rom_map_elem_t methods_table[] = {
    {MP_ROM_QSTR(MP_QSTR_exchange), MP_ROM_PTR(&exchange_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&close_obj)},
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&close_obj)},
};
static MP_DEFINE_CONST_DICT(methods, methods_table);
static MP_DEFINE_CONST_OBJ_TYPE(client_type, MP_QSTR_HTTPSClient, MP_TYPE_FLAG_NONE, locals_dict, &methods);
static const mp_rom_map_elem_t globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mm_https)},
    {MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&open_obj)},
};
static MP_DEFINE_CONST_DICT(globals, globals_table);
const mp_obj_module_t mm_https_module = {
    .base = {&mp_type_module}, .globals = (mp_obj_dict_t *)&globals,
};
MP_REGISTER_MODULE(MP_QSTR_mm_https, mm_https_module);
