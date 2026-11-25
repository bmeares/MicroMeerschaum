// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_littlefs.h"
#include "micromeerschaum/sqlite.h"

static void check(bool ok)
{
    if (!ok) abort();
}

#ifdef CONFIG_MM_SQLITE_EXAMPLE_FAIL_SYNC
static bool fail_next_sync;
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (fail_next_sync) {
        fail_next_sync = false;
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}
#endif

static const char key[] = "sensor\x1f" "reading\x1f";
static const mm_journal_config config = {
    {1,1,key,sizeof key-1}, MM_JOURNAL_ORIGIN, MM_DESTINATION, 2, 2, 0
};
static const char path[] = "/store/device.db";

static mm_sync_result write_value(mm_sqlite *db, int64_t value)
{
    uint8_t scratch[128];
    mm_pipe pipe;
    check(mm_pipe_open(&pipe,"sensor","reading",NULL,mm_sqlite_target(db),scratch,sizeof scratch)
          == MM_JOURNAL_OK);
    const mm_field fields[] = {{{"tick",4},{MM_VALUE_INT,{.integer=41}}},
                               {{"value",5},{MM_VALUE_INT,{.integer=value}}}};
    return mm_pipe_sync_rows(&pipe,&(mm_row){fields,2},1,0,1);
}

void app_main(void)
{
    const esp_vfs_littlefs_conf_t mount = {.base_path="/store",.partition_label="storage"};
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&mount));

    const mm_sqlite_definition *registration = NULL;
#ifdef CONFIG_MM_SQLITE_EXAMPLE_PROVISION
    static const mm_sqlite_column columns[] = {{"tick",MM_VALUE_INT,true},{"value",MM_VALUE_INT,false}};
    static const mm_sqlite_definition def = {columns,2,0,false};
    registration = &def;
#endif
    mm_sqlite *db;
    check(mm_sqlite_open(&db,path,&config,registration) == MM_JOURNAL_OK);
    uint64_t high, confirmed;
    check(mm_sqlite_positions(db,&high,&confirmed) == MM_JOURNAL_OK);
#ifdef CONFIG_MM_SQLITE_EXAMPLE_FAIL_SYNC
    uint64_t previous = high;
    fail_next_sync = true;
    mm_sync_result rejected = write_value(db, -1);
    check(!fail_next_sync && rejected.status != MM_JOURNAL_OK && rejected.confirmed == 0);
    mm_sqlite_close(db);
    check(mm_sqlite_open(&db,path,&config,NULL) == MM_JOURNAL_OK);
    check(mm_sqlite_positions(db,&high,&confirmed) == MM_JOURNAL_OK && high == previous);
    printf("Recovered after a failed SQLite fsync at sequence %llu\n",(unsigned long long)high);
#endif
    check(write_value(db,(int64_t)high+1).confirmed == 1);
    uint8_t original[256], retry[256];
    size_t len, retried;
    check(mm_sqlite_read(db,high+1,original,sizeof original,&len) == MM_JOURNAL_OK);
    mm_sqlite_close(db);
    check(mm_sqlite_open(&db,path,&config,NULL) == MM_JOURNAL_OK);
    check(mm_sqlite_read(db,high+1,retry,sizeof retry,&retried) == MM_JOURNAL_OK &&
          retried == len && !memcmp(original,retry,len));
    mm_value latest;
    check(mm_sqlite_get_sync_time(db,&(mm_sqlite_query){0},&latest) == MM_JOURNAL_OK);
    check(latest.type == MM_VALUE_INT && latest.as.integer == 41);
    check(mm_sqlite_positions(db,&high,&confirmed) == MM_JOURNAL_OK);
    printf("Recovered SQLite sequence %llu, latest tick 41\n", (unsigned long long)high);
    mm_sqlite_close(db);
    ESP_ERROR_CHECK(esp_vfs_littlefs_unregister("storage"));
}
