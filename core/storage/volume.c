// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include "volume.h"
#include "../src/le.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include <pthread.h>
#else
#include <sys/file.h>
#endif

/* Separate sectors for alternate commit pages. Active slots are never
 * overwritten during a transaction. Slot data is an ordinary MM segment. */
#define LEGACY_CHECKSUM 2044u
#define ROUTE_START 2052u
#define PAGE_SIZE 4096u
#define PAGE_STRIDE 4096u
#define ENTRY_START 384u
#define ENTRY_SIZE 20u
#define STATE_START 40u
#define CHECKSUM (PAGE_SIZE - 4u)
_Static_assert(STATE_START + MM_STATE_MAX <= ENTRY_START, "state exceeds reserved space");
_Static_assert(ENTRY_START + (MM_VOLUME_MAX_SLOTS + 1u) * ENTRY_SIZE <= CHECKSUM, "slot table exceeds page");

struct mm_volume {
    int fd;
    mm_volume_options opt;
    unsigned page;
    bool repair, routes_dirty;
    uint8_t manifest[PAGE_SIZE];
    mm_journal_barrier barrier;
    void *context;
#ifdef ESP_PLATFORM
    char *path;
    mm_volume *next;
#endif
};

#ifdef ESP_PLATFORM
/* ESP VFS has no flock. Applications use one canonical path per file. */
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static mm_volume *registry;
static bool claim(mm_volume *v)
{
    pthread_mutex_lock(&registry_lock);
    mm_volume *it = registry;
    while (it && strcmp(it->path, v->path)) it = it->next;
    if (!it) { v->next = registry; registry = v; }
    pthread_mutex_unlock(&registry_lock);
    return !it;
}
#endif

static size_t stride(const mm_volume_options *o)
{
    return ((size_t)o->segment_bytes + 511u) & ~(size_t)511u;
}
static off_t slot_offset(const mm_volume *v, unsigned slot)
{
    return (off_t)(2u * PAGE_STRIDE + slot * stride(&v->opt));
}
static uint8_t *entry(uint8_t *page, unsigned slot)
{
    return page + ENTRY_START + slot * ENTRY_SIZE;
}
static segment descriptor(const mm_volume *v, unsigned slot)
{
    const uint8_t *e = v->manifest + ENTRY_START + slot * ENTRY_SIZE;
    return (segment){{mm_get64(e), mm_get64(e+8)}, mm_get32(e+16)};
}
static int barrier(mm_volume *v)
{
    int rc;
    do { rc = v->barrier ? v->barrier(v->fd, v->context) : fsync(v->fd); }
    while (rc < 0 && errno == EINTR);
    return rc;
}

/* lseek + read/write is available on both embedded VFS drivers. Callers
 * serialize a handle, so the file position is private to this transaction. */
static mm_journal_status transfer(mm_volume *v, off_t at, uint8_t *read_buf,
                                  const uint8_t *write_buf, size_t size)
{
    if (lseek(v->fd, at, SEEK_SET) != at) return MM_JOURNAL_IO;
    while (size) {
        ssize_t n = read_buf ? read(v->fd, read_buf, size) : write(v->fd, write_buf, size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return errno == ENOSPC ? MM_JOURNAL_FULL : MM_JOURNAL_IO;
        if (!n) return read_buf ? MM_JOURNAL_CORRUPT : MM_JOURNAL_FULL;
        if (read_buf) read_buf += n;
        else write_buf += n;
        size -= (size_t)n;
    }
    return MM_JOURNAL_OK;
}

static mm_journal_status write_page(mm_volume *v, unsigned page)
{
    if (transfer(v, (off_t)(page * PAGE_STRIDE), NULL, v->manifest, PAGE_SIZE) != MM_JOURNAL_OK ||
        barrier(v) < 0) return MM_JOURNAL_UNCERTAIN;
    return MM_JOURNAL_OK;
}

mm_journal_status mm_volume_repair(mm_volume *v)
{
    if (!v->repair) return MM_JOURNAL_OK;
    mm_journal_status st = write_page(v, v->page ^ 1u);
    if (st == MM_JOURNAL_OK) v->repair = false;
    return st;
}

static size_t checksum_offset(const uint8_t *page)
{
    return page[4] == 1 ? LEGACY_CHECKSUM : CHECKSUM;
}

static mm_journal_status checkpoint(mm_volume *v)
{
    uint64_t epoch = mm_get64(v->manifest + 8);
    /* Once modifications to the RAM manifest start, any error requires reopen. */
    if (epoch == UINT64_MAX) return MM_JOURNAL_UNCERTAIN;
    mm_put64(v->manifest + 8, epoch + 1);
    size_t crc_at = checksum_offset(v->manifest);
    mm_put32(v->manifest + crc_at, mm_crc32(0, v->manifest, crc_at));
    v->page ^= 1u;
    mm_journal_status st = write_page(v, v->page);
    if (st != MM_JOURNAL_OK) return st;
    /* A completed commit has two copies of its latest allocator state. A
     * single corrupt page cannot roll the stream back to an old sequence. */
    v->repair = true;
    v->routes_dirty = false;
    return mm_volume_repair(v);
}

static bool valid_page(const uint8_t *p)
{
    if (memcmp(p, "MMJV", 4) || (p[4] != 1 && p[4] != 2) || p[5] || p[6] || p[7] ||
        !mm_get64(p + 8)) return false;
    size_t crc_at = checksum_offset(p);
    return mm_get32(p + crc_at) == mm_crc32(0, p, crc_at) &&
           (p[4] == 1 || mm_get32(p + ROUTE_START - 4) <= MM_ROUTE_BYTES);
}
static mm_journal_status restore(mm_volume *v)
{
    uint8_t *second = malloc(PAGE_SIZE);
    if (!second) return MM_JOURNAL_NOMEM;
    mm_journal_status st = transfer(v, 0, v->manifest, NULL, PAGE_SIZE);
    if (st == MM_JOURNAL_OK) st = transfer(v, PAGE_STRIDE, second, NULL, PAGE_SIZE);
    if (st == MM_JOURNAL_OK) {
        bool a = valid_page(v->manifest), b = valid_page(second);
        v->repair = !a || !b || memcmp(v->manifest, second, checksum_offset(v->manifest) + 4) != 0;
        if (!a && !b) st = MM_JOURNAL_CORRUPT;
        else if (a && b && v->repair && mm_get64(v->manifest + 8) == mm_get64(second + 8))
            st = MM_JOURNAL_CORRUPT;
        else if (b && (!a || mm_get64(second + 8) > mm_get64(v->manifest + 8))) {
            memcpy(v->manifest, second, PAGE_SIZE);
            v->page = 1;
        }
    }
    free(second);
    if (st != MM_JOURNAL_OK) return st;
    if (mm_get32(v->manifest + 16) != v->opt.segments ||
        mm_get32(v->manifest + 20) != v->opt.segment_bytes ||
        mm_get32(v->manifest + 24) != (uint32_t)v->opt.retention) return MM_JOURNAL_CONFLICT;
    if (mm_get32(v->manifest + 28) > MM_STATE_MAX) return MM_JOURNAL_CORRUPT;
    for (unsigned i = 0; i <= v->opt.segments; ++i) {
        segment s = descriptor(v, i);
        if ((!s.size && (s.range.first || s.range.last)) ||
            (s.size && (!s.range.first || s.range.first > s.range.last || s.size > v->opt.segment_bytes)))
            return MM_JOURNAL_CORRUPT;
    }
    if (mm_volume_count(v) > v->opt.segments) return MM_JOURNAL_CORRUPT;
    return MM_JOURNAL_OK;
}

static mm_journal_status initialize(mm_volume *v)
{
    /* Write, including unused slots, so a logical full condition leaves
     * physical space for its evidence. Do not create a sparse file. */
    off_t size = slot_offset(v, (unsigned)v->opt.segments + 1u);
    for (off_t at = 0; at < size; at += PAGE_SIZE) {
        size_t n = (size_t)(size - at);
        if (n > PAGE_SIZE) n = PAGE_SIZE;
        mm_journal_status st = transfer(v, at, NULL, v->manifest, n);
        if (st != MM_JOURNAL_OK) return st;
    }
    if (barrier(v) < 0) return MM_JOURNAL_UNCERTAIN;
    memcpy(v->manifest, "MMJV\1", 5);
    mm_put32(v->manifest + 16, v->opt.segments);
    mm_put32(v->manifest + 20, v->opt.segment_bytes);
    mm_put32(v->manifest + 24, v->opt.retention);
    return MM_JOURNAL_OK;
}

#ifndef ESP_PLATFORM
/* POSIX file fsync covers data and inode metadata. Its directory barrier
 * separately establishes the name of the image across initial provisioning. */
static mm_journal_status sync_parent(const char *path)
{
    char *directory = strdup(path);
    if (!directory) return MM_JOURNAL_NOMEM;
    char *slash = strrchr(directory, '/');
    if (slash == directory) directory[1] = 0;
    else if (slash) *slash = 0;
    int fd = open(slash ? directory : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(directory);
    if (fd < 0) return MM_JOURNAL_IO;
    int rc;
    do { rc = fsync(fd); } while (rc < 0 && errno == EINTR);
    if (close(fd) < 0 || rc < 0) return MM_JOURNAL_IO;
    return MM_JOURNAL_OK;
}
#endif

mm_journal_status mm_volume_open(mm_volume **out, const char *path, const mm_volume_options *opt,
                                 bool create)
{
    if (!out || !path || !opt || opt->segments < 2 || opt->segments > MM_VOLUME_MAX_SLOTS ||
        opt->segment_bytes < 512 || opt->segment_bytes > MM_JOURNAL_SEGMENT_MAX ||
        (opt->retention != MM_KEEP_OLD && opt->retention != MM_KEEP_NEW)) return MM_JOURNAL_INVALID;
    *out = NULL;
    mm_volume *v = calloc(1, sizeof(*v));
    if (!v) return MM_JOURNAL_NOMEM;
    v->opt = *opt;
    v->fd = -1;
#ifdef ESP_PLATFORM
    v->path = strdup(path);
    if (!v->path) { free(v); return MM_JOURNAL_NOMEM; }
    if (!claim(v)) { free(v->path); free(v); return MM_JOURNAL_BUSY; }
#endif
    bool fresh = false;
    if (create) {
        v->fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        fresh = v->fd >= 0;
    }
    if (!create || (!fresh && errno == EEXIST)) v->fd = open(path, O_RDWR);
    mm_journal_status st = MM_JOURNAL_OK;
    if (v->fd < 0) st = errno == ENOENT ? MM_JOURNAL_NOT_FOUND : MM_JOURNAL_IO;
#ifndef ESP_PLATFORM
    if (st == MM_JOURNAL_OK && flock(v->fd, LOCK_EX | LOCK_NB) < 0)
        st = errno == EWOULDBLOCK ? MM_JOURNAL_BUSY : MM_JOURNAL_IO;
#endif
    if (st == MM_JOURNAL_OK && fresh) st = initialize(v);
    if (st == MM_JOURNAL_OK && !fresh) {
        struct stat info;
        if (fstat(v->fd, &info) < 0 || barrier(v) < 0) st = MM_JOURNAL_IO;
        else if (!S_ISREG(info.st_mode)) st = MM_JOURNAL_INVALID;
        else {
            st = restore(v);
            if (st == MM_JOURNAL_OK && info.st_size != slot_offset(v, (unsigned)opt->segments + 1u))
                st = MM_JOURNAL_CORRUPT;
        }
    }
#ifndef ESP_PLATFORM
    if (st == MM_JOURNAL_OK) st = sync_parent(path);
#endif
    if (st != MM_JOURNAL_OK) { mm_volume_close(v); return st; }
    *out = v;
    return MM_JOURNAL_OK;
}

void mm_volume_close(mm_volume *v)
{
    if (!v) return;
    if (v->fd >= 0) close(v->fd);
#ifdef ESP_PLATFORM
    pthread_mutex_lock(&registry_lock);
    mm_volume **it = &registry;
    while (*it != v) it = &(*it)->next;
    *it = v->next;
    pthread_mutex_unlock(&registry_lock);
    free(v->path);
#endif
    free(v);
}
void mm_volume_set_barrier(mm_volume *v, mm_journal_barrier fn, void *context)
{
    v->barrier = fn;
    v->context = context;
}
size_t mm_volume_limit(const mm_volume *v) { return v->opt.segment_bytes; }
size_t mm_volume_capacity(const mm_volume *v) { return v->opt.segments; }
mm_retention mm_volume_retention(const mm_volume *v) { return v->opt.retention; }
uint64_t mm_volume_missed(const mm_volume *v) { return mm_get64(v->manifest + 32); }
size_t mm_volume_count(const mm_volume *v)
{
    size_t n = 0;
    for (unsigned i = 0; i <= v->opt.segments; ++i) n += descriptor(v, i).size != 0;
    return n;
}
segment mm_volume_segment(const mm_volume *v, size_t ordinal)
{
    for (unsigned i = 0; i <= v->opt.segments; ++i) {
        segment s = descriptor(v, i);
        if (s.size && ordinal-- == 0) return s;
    }
    return (segment){{0, 0}, 0};
}
const uint8_t *mm_volume_routes(mm_volume *v, size_t *len)
{
    *len = v->manifest[4] == 1 ? 0 : mm_get32(v->manifest + ROUTE_START - 4);
    return v->manifest + ROUTE_START;
}

void mm_volume_stage_routes(mm_volume *v, const uint8_t *data, size_t len)
{
    v->manifest[4] = 2;
    memcpy(v->manifest + ROUTE_START, data, len);
    mm_put32(v->manifest + ROUTE_START - 4, (uint32_t)len);
    v->routes_dirty = true;
}

mm_journal_status mm_volume_commit_routes(mm_volume *v)
{
    return v->routes_dirty ? checkpoint(v) : MM_JOURNAL_OK;
}

mm_journal_status mm_volume_state(mm_volume *v, uint8_t **out, size_t *size)
{
    *size = mm_get32(v->manifest + 28);
    if (!*size) return MM_JOURNAL_NOT_FOUND;
    *out = malloc(*size);
    if (!*out) return MM_JOURNAL_NOMEM;
    memcpy(*out, v->manifest + STATE_START, *size);
    return MM_JOURNAL_OK;
}
mm_journal_status mm_volume_save_state(mm_volume *v, const uint8_t *state, size_t size)
{
    if (size > MM_STATE_MAX) return MM_JOURNAL_INVALID;
    mm_put32(v->manifest + 28, (uint32_t)size);
    memcpy(v->manifest + STATE_START, state, size);
    return checkpoint(v);
}
mm_journal_status mm_volume_read(mm_volume *v, seq_range range, uint8_t **out, size_t *size)
{
    for (unsigned i = 0; i <= v->opt.segments; ++i) {
        segment s = descriptor(v, i);
        if (s.range.first != range.first || s.range.last != range.last) continue;
        uint8_t *buf = malloc(s.size);
        if (!buf) return MM_JOURNAL_NOMEM;
        mm_journal_status st = transfer(v, slot_offset(v, i), buf, NULL, s.size);
        if (st != MM_JOURNAL_OK) { free(buf); return st; }
        *out = buf;
        *size = s.size;
        return MM_JOURNAL_OK;
    }
    return MM_JOURNAL_NOT_FOUND;
}
static bool trim(mm_volume *v, uint64_t through)
{
    bool changed = false;
    for (unsigned i = 0; i <= v->opt.segments; ++i) {
        segment s = descriptor(v, i);
        if (s.size && s.range.last <= through) {
            memset(entry(v->manifest, i), 0, ENTRY_SIZE);
            changed = true;
        }
    }
    return changed;
}
mm_journal_status mm_volume_trim(mm_volume *v, uint64_t through)
{
    return trim(v, through) ? checkpoint(v) : MM_JOURNAL_OK;
}
mm_journal_status mm_volume_set_missed(mm_volume *v, uint64_t missed)
{
    mm_put64(v->manifest + 32, missed);
    return checkpoint(v);
}
mm_journal_status mm_volume_publish(mm_volume *v, seq_range range, const uint8_t *header, size_t h,
                                    const uint8_t *records, size_t n, uint64_t remove_through,
                                    uint64_t missed)
{
    if (h > v->opt.segment_bytes || n > v->opt.segment_bytes - h) return MM_JOURNAL_TOO_SMALL;
    unsigned free_slot = 0, kept = 0;
    for (unsigned i = 0; i <= v->opt.segments; ++i) {
        segment s = descriptor(v, i);
        if (!s.size) free_slot = i;
        else if (s.range.last > remove_through) ++kept;
    }
    if (kept >= v->opt.segments) return MM_JOURNAL_FULL;
    off_t at = slot_offset(v, free_slot);
    mm_journal_status st = transfer(v, at, NULL, header, h);
    if (st == MM_JOURNAL_OK) st = transfer(v, at + (off_t)h, NULL, records, n);
    /* A physical write failure may put the VFS descriptor into an error
     * state. Reopen before issuing another write, even to the loss reserve. */
    if (st != MM_JOURNAL_OK || barrier(v) < 0) return MM_JOURNAL_UNCERTAIN;
    trim(v, remove_through);
    uint8_t *e = entry(v->manifest, free_slot);
    mm_put64(e, range.first);
    mm_put64(e + 8, range.last);
    mm_put32(e + 16, (uint32_t)(h + n));
    mm_put64(v->manifest + 32, missed);
    return checkpoint(v);
}
