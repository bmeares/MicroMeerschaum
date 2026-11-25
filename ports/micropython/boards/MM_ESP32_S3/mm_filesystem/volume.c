// SPDX-License-Identifier: Apache-2.0
#include "mm_board_volume.h"
#include "esp_littlefs.h"

esp_err_t mm_board_mount(bool format_if_mount_failed)
{
    if (esp_littlefs_mounted("mmdata")) return ESP_OK;
    const esp_vfs_littlefs_conf_t config = {
        .base_path = "/mm", .partition_label = "mmdata",
        .format_if_mount_failed = format_if_mount_failed
    };
    return esp_vfs_littlefs_register(&config);
}
