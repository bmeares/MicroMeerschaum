// SPDX-License-Identifier: Apache-2.0
#include <assert.h>
#include <stdio.h>
#include "esp_littlefs.h"
#include "micromeerschaum/sqlite.h"

void app_main(void)
{
    const esp_vfs_littlefs_conf_t mount = {.base_path="/store",.partition_label="storage"};
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&mount));
    static const char key[] = "sensor\x1f" "reading\x1f";
    const mm_journal_config config = {{1,1,key,sizeof key-1},MM_JOURNAL_ORIGIN,MM_DESTINATION,2,2,0};
    const mm_sqlite_column columns[] = {{"tick",MM_VALUE_INT,true},{"value",MM_VALUE_INT,false}};
    const mm_sqlite_definition def = {columns,2,0,false};
    mm_sqlite *db;
    assert(mm_sqlite_open(&db,"/store/device.db",&config,&def) == MM_JOURNAL_OK);
    uint64_t high, confirmed;
    assert(mm_sqlite_positions(db,&high,&confirmed) == MM_JOURNAL_OK);
    uint8_t scratch[128];
    mm_pipe p;
    assert(mm_pipe_open(&p,"sensor","reading",NULL,mm_sqlite_target(db),scratch,sizeof scratch)==MM_JOURNAL_OK);
    const mm_field fields[] = {{{"tick",4},{MM_VALUE_INT,{.integer=41}}},
                              {{"value",5},{MM_VALUE_INT,{.integer=(int64_t)high+1}}}};
    assert(mm_pipe_sync_rows(&p,&(mm_row){fields,2},1,0,1).confirmed == 1);
    mm_sqlite_close(db);
    assert(mm_sqlite_open(&db,"/store/device.db",&config,NULL) == MM_JOURNAL_OK);
    mm_value latest;
    assert(mm_sqlite_get_sync_time(db,&(mm_sqlite_query){0},&latest) == MM_JOURNAL_OK);
    assert(latest.type == MM_VALUE_INT && latest.as.integer == 41);
    assert(mm_sqlite_positions(db,&high,&confirmed) == MM_JOURNAL_OK);
    printf("Recovered SQLite sequence %llu, latest tick 41\n", (unsigned long long)high);
    mm_sqlite_close(db);
    ESP_ERROR_CHECK(esp_vfs_littlefs_unregister("storage"));
}
