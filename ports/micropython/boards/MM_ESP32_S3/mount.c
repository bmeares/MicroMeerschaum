// SPDX-License-Identifier: Apache-2.0
#include "esp_vfs_fat.h"
#include "py/runtime.h"

// MicroPython's own filesystem lives on the separate vfs partition. The
// native library needs a mounted ESP-IDF VFS so it can check fsync errors.
static wl_handle_t storage = WL_INVALID_HANDLE;

static mp_obj_t mount_volume(size_t n_args, const mp_obj_t *args) {
    if (storage != WL_INVALID_HANDLE) return mp_const_none;
    const esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = n_args && mp_obj_is_true(args[0]),
        .max_files = 16,
        .allocation_unit_size = 4096,
    };
    wl_handle_t mounted = WL_INVALID_HANDLE;
    esp_err_t error = esp_vfs_fat_spiflash_mount_rw_wl("/mm", "mmdata", &config, &mounted);
    if (error != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("mount mmdata: %s"), esp_err_to_name(error));
    }
    storage = mounted;
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
