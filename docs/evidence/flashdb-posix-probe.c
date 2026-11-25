// SPDX-License-Identifier: Apache-2.0
// Build against the revisions and configuration in ../architecture-evaluation.md.
// Run in a fresh temporary directory. Each case uses a separate disposable store.
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <flashdb.h>
#include <micromeerschaum/mm.h>

static bool fail_sync;
static unsigned failed_syncs;

int __real_fsync(int fd);
int __wrap_fsync(int fd) {
    if (fail_sync) {
        ++failed_syncs;
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

static fdb_time_t initial_time(void) {
    return 1;
}

static fdb_err_t append_frame(fdb_tsdb_t db, uint64_t seq) {
    uint8_t frame[MM_RECORD_HEADER_SIZE + 39] = {0};
    struct fdb_blob blob;
    if (mm_record_header(frame, MM_KIND_DATA, seq, seq, 1, frame + MM_RECORD_HEADER_SIZE, 39) != MM_OK) {
        abort();
    }
    return fdb_tsl_append_with_ts(db, fdb_blob_make(&blob, frame, sizeof(frame)), (fdb_time_t)seq);
}

struct retirement {
    fdb_tsdb_t db;
    size_t marked;
};

static bool mark_deleted(fdb_tsl_t tsl, void *arg) {
    struct retirement *retirement = arg;
    if (fdb_tsl_set_status(retirement->db, tsl, FDB_TSL_DELETED) != FDB_NO_ERR) {
        return true;
    }
    ++retirement->marked;
    return false;
}

static bool first_seq(fdb_tsl_t tsl, void *arg) {
    *(fdb_time_t *)arg = tsl->time;
    return true;
}

static int probe(const char *directory, bool rollover) {
    struct fdb_tsdb db = {0};
    uint32_t sector_size = 4096, store_size = 4 * sector_size;
    bool file_mode = true;
    if (mkdir(directory, 0700) != 0) {
        perror(directory);
        return 1;
    }
    fdb_tsdb_control(&db, FDB_TSDB_CTRL_SET_SEC_SIZE, &sector_size);
    fdb_tsdb_control(&db, FDB_TSDB_CTRL_SET_MAX_SIZE, &store_size);
    fdb_tsdb_control(&db, FDB_TSDB_CTRL_SET_FILE_MODE, &file_mode);
    if (fdb_tsdb_init(&db, "probe", directory, initial_time, 71, NULL) != FDB_NO_ERR) {
        return 1;
    }
    fdb_tsdb_control(&db, FDB_TSDB_CTRL_SET_ROLLOVER, &rollover);
    if (strcmp(directory, "sync-error") == 0) {
        if (append_frame(&db, 1) != FDB_NO_ERR) return 1;
        fail_sync = true;
        fdb_err_t result = append_frame(&db, 2);
        fail_sync = false;
        printf("sync-error: failed_fsync=%u append=%d (success=%d)\n", failed_syncs, result, FDB_NO_ERR);
    } else {
        size_t capacity = fdb_tsl_max_blob_count(&db);
        for (size_t seq = 1; seq <= capacity; ++seq) {
            if (append_frame(&db, seq) != FDB_NO_ERR) return 1;
        }
        fdb_err_t result = append_frame(&db, capacity + 1);
        if (rollover) {
            fdb_time_t first = 0;
            fdb_tsl_iter(&db, first_seq, &first);
            printf("rollover: capacity=%zu append=%d oldest=%lld\n", capacity, result, (long long)first);
        } else {
            struct retirement retirement = {&db, 0};
            fdb_tsl_iter(&db, mark_deleted, &retirement);
            size_t verified = fdb_tsl_query_count(&db, 1, (fdb_time_t)capacity, FDB_TSL_DELETED);
            fdb_err_t retry = append_frame(&db, capacity + 1);
            printf("no-rollover: capacity=%zu append=%d marked=%zu verified=%zu retry=%d (full=%d)\n",
                   capacity, result, retirement.marked, verified, retry, FDB_SAVED_FULL);
        }
    }
    fdb_tsdb_deinit(&db);
    return 0;
}

int main(void) {
    if (probe("sync-error", false) != 0) return 1;
    if (probe("no-rollover", false) != 0) return 1;
    return probe("rollover", true);
}
