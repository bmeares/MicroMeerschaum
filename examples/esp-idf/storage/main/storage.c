// SPDX-License-Identifier: Apache-2.0
#include <inttypes.h>
#include "esp_log.h"
#include "micromeerschaum/journal.h"
#include "sdkconfig.h"
#ifdef CONFIG_MM_EXAMPLE_SDMMC
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#else
#include "esp_littlefs.h"
#endif

static esp_err_t mount_medium(void)
{
#ifdef CONFIG_MM_EXAMPLE_SDMMC
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = CONFIG_MM_EXAMPLE_SD_CLK;
    slot.cmd = CONFIG_MM_EXAMPLE_SD_CMD;
    slot.d0 = CONFIG_MM_EXAMPLE_SD_D0;
    const esp_vfs_fat_sdmmc_mount_config_t mount = {.max_files = 4, .disk_status_check_enable = true};
    sdmmc_card_t *card;
    return esp_vfs_fat_sdmmc_mount("/store", &host, &slot, &mount, &card);
#else
    const esp_vfs_littlefs_conf_t mount = {.base_path = "/store", .partition_label = "storage"};
    return esp_vfs_littlefs_register(&mount);
#endif
}

void app_main(void)
{
    if (mount_medium() != ESP_OK) { ESP_LOGE("storage", "Mount failed"); return; }
    static const char key[] = "sensor\x1ftemp\x1f";
    const mm_journal_config pipe = {{1, 5, key, sizeof(key)-1}, MM_JOURNAL_ORIGIN,
                                    MM_DESTINATION, 2, 2, 0};
    const mm_volume_options options = {16, 4096, MM_KEEP_OLD};
#ifdef CONFIG_MM_EXAMPLE_PROVISION
    const bool create = true;
#else
    const bool create = false;
#endif
    mm_journal *j;
    mm_journal_status st = mm_journal_open_volume(&j, "/store/PIPE.MM", &pipe, &options, create);
    if (st != MM_JOURNAL_OK) { ESP_LOGE("storage", "Open: %s", mm_journal_status_name(st)); return; }
    const mm_sample readings[] = {{MM_KIND_DATA, 1000, 1, "A", 1}, {MM_KIND_DATA, 2000, 1, "B", 1}};
    uint64_t seq;
    st = mm_journal_append_group(j, readings, 2, &seq);
    mm_journal_close(j);
    if (st != MM_JOURNAL_OK) { ESP_LOGE("storage", "Commit: %s", mm_journal_status_name(st)); return; }
    st = mm_journal_open_volume(&j, "/store/PIPE.MM", &pipe, &options, false);
    if (st != MM_JOURNAL_OK) { ESP_LOGE("storage", "Reopen: %s", mm_journal_status_name(st)); return; }
    uint8_t frame[MM_RECORD_HEADER_SIZE + 1];
    size_t used;
    st = mm_journal_read(j, seq + 1, frame, sizeof(frame), &used);
    if (st == MM_JOURNAL_OK && used == sizeof(frame) && frame[MM_RECORD_HEADER_SIZE] == 'B')
        ESP_LOGI("storage", "Recovered group %" PRIu64 "..%" PRIu64, seq, seq + 1);
    else ESP_LOGE("storage", "Read-back failed: %s", mm_journal_status_name(st));
    mm_journal_close(j);
}
