// SPDX-License-Identifier: Apache-2.0
/* SQLite VFS for a mounted, writable ESP LittleFS volume. Each canonical
 * absolute database path has a single owner until close. SQLite handles
 * crash recovery with a persistent rollback journal on the same volume. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <sqlite3.h>
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MM_SQL_PATH 255
typedef struct sql_file {
    sqlite3_file base;
    int fd;
    bool main;
    const char *path; /* xOpen name is valid through xClose. */
    struct sql_file *next;
} sql_file;

static sql_file *owners;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;

static bool claim(sql_file *f)
{
    portENTER_CRITICAL(&guard);
    sql_file *p = owners;
    while (p && strcmp(p->path, f->path)) p = p->next;
    if (!p) { f->next = owners; owners = f; }
    portEXIT_CRITICAL(&guard);
    return p == NULL;
}

static void release(sql_file *f)
{
    if (!f->main) return;
    portENTER_CRITICAL(&guard);
    sql_file **p = &owners;
    while (*p && *p != f) p = &(*p)->next;
    if (*p) *p = f->next;
    portEXIT_CRITICAL(&guard);
}

static int file_close(sqlite3_file *file)
{
    sql_file *f = (sql_file *)file;
    int rc = close(f->fd);
    release(f);
    return rc ? SQLITE_IOERR_CLOSE : SQLITE_OK;
}

static bool seek_to(sql_file *f, sqlite3_int64 pos)
{
    off_t off = (off_t)pos;
    return pos >= 0 && (sqlite3_int64)off == pos && lseek(f->fd, off, SEEK_SET) == off;
}

static int file_read(sqlite3_file *file, void *buf, int n, sqlite3_int64 pos)
{
    sql_file *f = (sql_file *)file;
    if (!seek_to(f, pos)) return SQLITE_IOERR_SEEK;
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t got = read(f->fd, p, (size_t)n);
        if (got < 0) { if (errno == EINTR) continue; return SQLITE_IOERR_READ; }
        if (!got) { memset(p, 0, (size_t)n); return SQLITE_IOERR_SHORT_READ; }
        p += got; n -= (int)got;
    }
    return SQLITE_OK;
}

static int file_write(sqlite3_file *file, const void *buf, int n, sqlite3_int64 pos)
{
    sql_file *f = (sql_file *)file;
    if (!seek_to(f, pos)) return SQLITE_IOERR_SEEK;
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t wrote = write(f->fd, p, (size_t)n);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return wrote < 0 && errno == ENOSPC ? SQLITE_FULL : SQLITE_IOERR_WRITE;
        p += wrote; n -= (int)wrote;
    }
    return SQLITE_OK;
}

static int file_truncate(sqlite3_file *file, sqlite3_int64 size)
{
    off_t n = (off_t)size;
    return size >= 0 && (sqlite3_int64)n == size && ftruncate(((sql_file *)file)->fd, n) == 0
         ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
}

static int file_sync(sqlite3_file *file, int flags)
{
    (void)flags;
    return fsync(((sql_file *)file)->fd) == 0 ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

static int file_size(sqlite3_file *file, sqlite3_int64 *out)
{
    struct stat s;
    if (fstat(((sql_file *)file)->fd, &s) != 0) return SQLITE_IOERR_FSTAT;
    *out = s.st_size;
    return SQLITE_OK;
}

/* A second connection cannot open the database while we hold its owner slot. */
static int file_lock(sqlite3_file *f, int level) { (void)f; (void)level; return SQLITE_OK; }
static int file_reserved(sqlite3_file *f, int *out) { (void)f; *out = 0; return SQLITE_OK; }
static int file_control(sqlite3_file *f, int op, void *arg)
{ (void)f; (void)op; (void)arg; return SQLITE_NOTFOUND; }
static int file_sector(sqlite3_file *f) { (void)f; return 4096; }
static int file_characteristics(sqlite3_file *f) { (void)f; return 0; }

static const sqlite3_io_methods io = {
    .iVersion=1, .xClose=file_close, .xRead=file_read, .xWrite=file_write,
    .xTruncate=file_truncate, .xSync=file_sync, .xFileSize=file_size,
    .xLock=file_lock, .xUnlock=file_lock, .xCheckReservedLock=file_reserved,
    .xFileControl=file_control, .xSectorSize=file_sector, .xDeviceCharacteristics=file_characteristics
};

static int vfs_open(sqlite3_vfs *v, const char *name, sqlite3_file *file, int flags, int *out)
{
    (void)v;
    sql_file *f = (sql_file *)file;
    memset(f, 0, sizeof *f);
    if (!name || !(flags & (SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_MAIN_JOURNAL)) ||
        (flags & SQLITE_OPEN_DELETEONCLOSE)) return SQLITE_CANTOPEN;
    f->path = name;
    f->main = (flags & SQLITE_OPEN_MAIN_DB) != 0;
    if (f->main && !claim(f)) return SQLITE_BUSY;
    int mode = (flags & SQLITE_OPEN_READWRITE) ? O_RDWR : O_RDONLY;
    if (flags & SQLITE_OPEN_CREATE) mode |= O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE) mode |= O_EXCL;
    f->fd = open(name, mode, 0600);
    if (f->fd < 0) { int e = errno; release(f); return e == ENOSPC ? SQLITE_FULL : SQLITE_CANTOPEN; }
    f->base.pMethods = &io;
    if (out) *out = flags;
    return SQLITE_OK;
}

static int vfs_delete(sqlite3_vfs *v, const char *name, int sync_dir)
{
    (void)v; (void)sync_dir; /* LittleFS commits its directory mutation in lfs_remove. */
    if (!unlink(name)) return SQLITE_OK;
    return errno == ENOENT ? SQLITE_IOERR_DELETE_NOENT : SQLITE_IOERR_DELETE;
}

static int vfs_access(sqlite3_vfs *v, const char *name, int flags, int *out)
{
    (void)v; (void)flags; /* This VFS owns a writable mount without per-file permissions. */
    struct stat s;
    if (!stat(name, &s)) { *out = 1; return SQLITE_OK; }
    *out = 0;
    return errno == ENOENT ? SQLITE_OK : SQLITE_IOERR_ACCESS;
}

static int vfs_path(sqlite3_vfs *v, const char *name, int n, char *out)
{
    (void)v;
    size_t len = strlen(name);
    if (name[0] != '/' || len >= (size_t)n || name[len-1]=='/' ||
        strstr(name,"//") || strstr(name,"/./") || strstr(name,"/../") ||
        (len >= 2 && strcmp(name+len-2,"/.")==0) ||
        (len >= 3 && strcmp(name+len-3,"/..")==0)) return SQLITE_CANTOPEN;
    memcpy(out,name,len+1);
    return SQLITE_OK;
}

static int vfs_random(sqlite3_vfs *v, int n, char *buf)
{ (void)v; esp_fill_random(buf, (size_t)n); return n; }
static int vfs_sleep(sqlite3_vfs *v, int us)
{ (void)v; esp_rom_delay_us((uint32_t)us); return us; }
static int vfs_time(sqlite3_vfs *v, double *out)
{
    (void)v;
    struct timeval t;
    if (gettimeofday(&t, NULL)) return SQLITE_ERROR;
    *out = 2440587.5 + ((double)t.tv_sec + (double)t.tv_usec / 1e6) / 86400.0;
    return SQLITE_OK;
}

static sqlite3_vfs vfs = {
    .iVersion=1, .szOsFile=sizeof(sql_file), .mxPathname=MM_SQL_PATH, .zName="mm-littlefs",
    .xOpen=vfs_open,.xDelete=vfs_delete,.xAccess=vfs_access,.xFullPathname=vfs_path,
    .xRandomness=vfs_random,.xSleep=vfs_sleep,.xCurrentTime=vfs_time
};
int sqlite3_os_init(void) { return sqlite3_vfs_register(&vfs, 1); }
int sqlite3_os_end(void) { return SQLITE_OK; }
