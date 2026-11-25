// SPDX-License-Identifier: Apache-2.0
#include "mm_board_volume.h"
#include "py/runtime.h"

// MicroPython's own filesystem lives on the separate vfs partition. The
// native library needs a mounted ESP-IDF VFS so it can check fsync errors.

static mp_obj_t mount_volume(size_t n_args, const mp_obj_t *args) {
    esp_err_t error = mm_board_mount(n_args && mp_obj_is_true(args[0]));
    if (error != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("mount mmdata: %s"), esp_err_to_name(error));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mount_obj, 0, 1, mount_volume);

static const mp_rom_map_elem_t globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__mmstorage)},
    {MP_ROM_QSTR(MP_QSTR_mount), MP_ROM_PTR(&mount_obj)},
};
static MP_DEFINE_CONST_DICT(globals, globals_table);
const mp_obj_module_t mmstorage_module = {
    .base = {&mp_type_module}, .globals = (mp_obj_dict_t *)&globals,
};
MP_REGISTER_MODULE(MP_QSTR__mmstorage, mmstorage_module);
