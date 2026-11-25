// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include "micromeerschaum/journal.h"
#include "../src/le.h"
#include "volume.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef ESP_PLATFORM
#include <sys/file.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#define STATE_FILE "journal.meta"
#define PENDING_FILE ".journal.pending"
#define STATE_MAX MM_STATE_MAX
#define NAME_SIZE 40u


struct mm_journal {
    int dir;
    mm_volume *volume;
    mm_journal_config config;
    char key[MM_KEY_MAX];
    bool uncertain;
    uint64_t confirmed, high, bytes;
    segment *segments;
    size_t count, capacity;
    seq_range *coverage;         /* Sorted disjoint ranges, including declared loss. */
    size_t ranges, range_capacity;
    uint8_t *cache;              /* One bounded segment for sequential range reads. */
    segment cached;
    size_t offset;
    uint64_t at_seq;
    mm_journal_barrier barrier;
    void *context;
};

#ifndef ESP_PLATFORM
static void segment_name(char name[NAME_SIZE], seq_range range)
{
    (void)snprintf(name, NAME_SIZE, "%016" PRIx64 "-%016" PRIx64 ".seg", range.first, range.last);
}

static bool parse_name(const char *name, seq_range *range)
{
    char canonical[NAME_SIZE];
    if (sscanf(name, "%16" SCNx64 "-%16" SCNx64 ".seg", &range->first, &range->last) != 2 ||
        range->first == 0 || range->last < range->first) return false;
    segment_name(canonical, *range);
    return strcmp(name, canonical) == 0;
}

static mm_journal_status io_error(void)
{
    return errno == ENOSPC || errno == EDQUOT ? MM_JOURNAL_FULL : MM_JOURNAL_IO;
}

#endif

static mm_journal_status uncertain(mm_journal *j)
{
    j->uncertain = true;
    return MM_JOURNAL_UNCERTAIN;
}

#ifndef ESP_PLATFORM
static int sync_fd(mm_journal *j, int fd)
{
    int rc;
    do { rc = j->barrier ? j->barrier(fd, j->context) : fsync(fd); } while (rc < 0 && errno == EINTR);
    return rc;
}

static mm_journal_status write_all(int fd, const uint8_t *data, size_t size)
{
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return io_error();
        if (n == 0) return MM_JOURNAL_IO;
        data += n;
        size -= (size_t)n;
    }
    return MM_JOURNAL_OK;
}

/* Only a complete, synced file gets a public name. The directory barrier
 * makes that name durable. An uncertain acknowledgement always requires reopen. */
static mm_journal_status publish(mm_journal *j, const char *name, const uint8_t *header,
                                 size_t header_size, const uint8_t *data, size_t size)
{
    int fd = openat(j->dir, PENDING_FILE, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return io_error();
    mm_journal_status st = write_all(fd, header, header_size);
    if (st == MM_JOURNAL_OK) st = write_all(fd, data, size);
    if (st == MM_JOURNAL_OK && sync_fd(j, fd) < 0) st = uncertain(j);
    if (close(fd) < 0 && st == MM_JOURNAL_OK) st = uncertain(j);
    if (st != MM_JOURNAL_OK) return st;
    if (renameat(j->dir, PENDING_FILE, j->dir, name) < 0)
        return errno == EIO ? uncertain(j) : io_error();
    return sync_fd(j, j->dir) < 0 ? uncertain(j) : MM_JOURNAL_OK;
}

/* Read a stable file under the directory lock. Cap the allocation before reading. */
static mm_journal_status load_file(mm_journal *j, const char *name, size_t limit,
                                   uint8_t **out, size_t *size)
{
    int fd = openat(j->dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? MM_JOURNAL_NOT_FOUND : MM_JOURNAL_IO;
    struct stat info;
    mm_journal_status st = MM_JOURNAL_OK;
    uint8_t *buf = NULL;
    if (fstat(fd, &info) < 0) st = MM_JOURNAL_IO;
    else if (!S_ISREG(info.st_mode) || info.st_size <= 0 || (uintmax_t)info.st_size > limit)
        st = MM_JOURNAL_CORRUPT;
    if (st == MM_JOURNAL_OK) {
        *size = (size_t)info.st_size;
        buf = malloc(*size);
        if (!buf) st = MM_JOURNAL_NOMEM;
    }
    size_t offset = 0;
    while (st == MM_JOURNAL_OK && offset < *size) {
        ssize_t n = read(fd, buf + offset, *size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) st = MM_JOURNAL_IO;
        else if (n == 0) st = MM_JOURNAL_CORRUPT;
        else offset += (size_t)n;
    }
    if (close(fd) < 0 && st == MM_JOURNAL_OK) st = MM_JOURNAL_IO;
    if (st == MM_JOURNAL_OK) *out = buf;
    else free(buf);
    return st;
}

#endif

/* A scope and its progress are replaced together. The floor survives deletion
 * of every confirmed segment. Missing metadata on reopen is never initialized. */
static mm_journal_status store_state(mm_journal *j, uint64_t confirmed)
{
    uint8_t state[STATE_MAX] = {'M','M','J','S',1,0,0,0};
    const mm_journal_config *c = &j->config;
    state[5] = (uint8_t)c->mode;
    state[6] = (uint8_t)c->role;
    mm_put64(state + 8, c->issuer);
    mm_put64(state + 16, c->destination);
    mm_put64(state + 24, confirmed);
    size_t n = mm_stream_encode(state + 32, MM_STREAM_MAX, &c->stream);
    if (!n) return MM_JOURNAL_INVALID;
    mm_put32(state + 32 + n, mm_crc32(0, state, 32 + n));
    if (j->volume) return mm_volume_save_state(j->volume, state, 36 + n);
#ifndef ESP_PLATFORM
    return publish(j, STATE_FILE, state, 36 + n, NULL, 0);
#else
    return MM_JOURNAL_INVALID;
#endif
}

static mm_journal_status load_state(mm_journal *j)
{
    uint8_t *buf;
    size_t n = 0;
    mm_journal_status st;
    if (j->volume) st = mm_volume_state(j->volume, &buf, &n);
#ifndef ESP_PLATFORM
    else st = load_file(j, STATE_FILE, STATE_MAX, &buf, &n);
#else
    else return MM_JOURNAL_INVALID;
#endif
    if (st != MM_JOURNAL_OK) return st;
    mm_stream stream;
    const mm_journal_config *c = &j->config;
    if (n <= 36 || memcmp(buf, "MMJS\1", 5) || buf[7] ||
        mm_get32(buf + n - 4) != mm_crc32(0, buf, n - 4) ||
        mm_stream_decode(buf + 32, n - 36, &stream) != MM_OK)
        st = MM_JOURNAL_CORRUPT;
    else if (buf[5] != (uint8_t)c->mode || buf[6] != (uint8_t)c->role ||
             mm_get64(buf + 8) != c->issuer || mm_get64(buf + 16) != c->destination ||
             !mm_stream_equal(&stream, &c->stream))
        st = MM_JOURNAL_CONFLICT;
    else {
        j->confirmed = mm_get64(buf + 24);
        if (c->mode == MM_JOURNAL_RECEIVER && j->confirmed) st = MM_JOURNAL_CORRUPT;
        j->high = j->confirmed;
    }
    free(buf);
    return st;
}

static void *grow(void *ptr, size_t *capacity, size_t elem)
{
    size_t n = *capacity ? *capacity * 2 : 8;
    if (n <= *capacity || n > SIZE_MAX / elem) return NULL;
    void *larger = realloc(ptr, n * elem);
    if (larger) *capacity = n;
    return larger;
}

static bool separated(uint64_t last, uint64_t first)
{
    return last < first && first - last > 1;
}

static mm_journal_status account(mm_journal *j, seq_range r)
{
    size_t left = 0;
    while (left < j->ranges && separated(j->coverage[left].last, r.first)) ++left;
    size_t right = left;
    while (right < j->ranges && !separated(r.last, j->coverage[right].first)) {
        if (j->coverage[right].first < r.first) r.first = j->coverage[right].first;
        if (j->coverage[right].last > r.last) r.last = j->coverage[right].last;
        ++right;
    }
    if (left == right && j->ranges == j->range_capacity) {
        void *p = grow(j->coverage, &j->range_capacity, sizeof(*j->coverage));
        if (!p) return MM_JOURNAL_NOMEM;
        j->coverage = p;
    }
    memmove(j->coverage + left + 1, j->coverage + right,
            (j->ranges - right) * sizeof(*j->coverage));
    j->coverage[left] = r;
    j->ranges = j->ranges - (right - left) + 1;
    return MM_JOURNAL_OK;
}

static mm_journal_status scan_segment(mm_journal *j, seq_range r, const uint8_t *buf,
                                      size_t n, size_t *header)
{
    mm_scan scan;
    if (mm_segment_scan(buf, n, &scan) != MM_OK || scan.stop != MM_END ||
        !scan.records || scan.first_seq != r.first || scan.last_seq != r.last ||
        scan.key_len != j->config.stream.key_len || memcmp(scan.key, j->key, scan.key_len))
        return MM_JOURNAL_CORRUPT;
    *header = scan.header_size;
    return MM_JOURNAL_OK;
}

/* First index with segment.first > seq. On disk, runs never overlap. */
static size_t upper_bound(const mm_journal *j, uint64_t seq)
{
    size_t lo = 0, hi = j->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (j->segments[mid].range.first <= seq) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static const segment *containing(const mm_journal *j, uint64_t seq)
{
    size_t i = upper_bound(j, seq);
    return i && j->segments[i-1].range.last >= seq ? &j->segments[i-1] : NULL;
}

/* Returns a view into the owned single-segment cache. Byte comparison during
 * overlapping retries uses the exact frame, including boot, time and CRC. */
static mm_journal_status load_segment(mm_journal *j, seq_range r, uint8_t **buf, size_t *size)
{
    if (j->volume) return mm_volume_read(j->volume, r, buf, size);
#ifndef ESP_PLATFORM
    char name[NAME_SIZE];
    segment_name(name, r);
    return load_file(j, name, MM_JOURNAL_SEGMENT_MAX, buf, size);
#else
    return MM_JOURNAL_INVALID;
#endif
}

static mm_journal_status lookup(mm_journal *j, uint64_t seq, const uint8_t **out, size_t *used)
{
    if (seq > j->high) return MM_JOURNAL_NOT_FOUND;
    if (!j->cache || seq < j->cached.range.first || seq > j->cached.range.last) {
        const segment *found = containing(j, seq);
        if (!found) return MM_JOURNAL_NOT_FOUND;
        segment s = *found;
        free(j->cache);
        j->cache = NULL;
        size_t size = 0;
        mm_journal_status st = load_segment(j, s.range, &j->cache, &size);
        if (st != MM_JOURNAL_OK) return st == MM_JOURNAL_NOT_FOUND ? MM_JOURNAL_CORRUPT : st;
        if (size != s.size || scan_segment(j, s.range, j->cache, size, &j->offset) != MM_JOURNAL_OK) {
            free(j->cache);
            j->cache = NULL;
            return MM_JOURNAL_CORRUPT;
        }
        j->cached = s;
        j->at_seq = s.range.first;
    }
    if (seq < j->at_seq) {
        j->at_seq = j->cached.range.first;
        j->offset = 28u + j->config.stream.key_len;
    }
    while (j->at_seq < seq) {
        j->offset += MM_RECORD_HEADER_SIZE + (size_t)mm_get32(j->cache + j->offset + 4);
        ++j->at_seq;
    }
    *out = j->cache + j->offset;
    *used = MM_RECORD_HEADER_SIZE + (size_t)mm_get32(*out + 4);
    return MM_JOURNAL_OK;
}

mm_journal_status mm_journal_read(mm_journal *j, uint64_t seq, uint8_t *out, size_t cap, size_t *used)
{
    if (!j || !seq || !used || (!out && cap)) return MM_JOURNAL_INVALID;
    if (j->uncertain) return MM_JOURNAL_UNCERTAIN;
    const uint8_t *frame;
    mm_journal_status st = lookup(j, seq, &frame, used);
    if (st != MM_JOURNAL_OK) return st;
    if (cap < *used) return MM_JOURNAL_TOO_SMALL;
    memcpy(out, frame, *used);
    return MM_JOURNAL_OK;
}

mm_journal_status mm_journal_next(mm_journal *j, uint64_t from, uint64_t *seq)
{
    if (!j || !from || !seq) return MM_JOURNAL_INVALID;
    if (j->uncertain) return MM_JOURNAL_UNCERTAIN;
    if (containing(j, from)) { *seq = from; return MM_JOURNAL_OK; }
    size_t i = upper_bound(j, from);
    if (i == j->count) return MM_JOURNAL_NOT_FOUND;
    *seq = j->segments[i].range.first;
    return MM_JOURNAL_OK;
}

mm_journal_status mm_journal_delivery_start(mm_journal *j, uint64_t from, uint64_t *seq)
{
    if (!j || !seq || !from) return MM_JOURNAL_INVALID;
    if (j->uncertain) return MM_JOURNAL_UNCERTAIN;
    if (containing(j, from)) { *seq = from; return MM_JOURNAL_OK; }
    /* A tiny batch may carry only one record. Deliver evidence first so the
     * receipt can advance across the hole before sending its successors. */
    for (size_t i = j->count; i > 0; --i) {
        seq_range r = j->segments[i - 1].range;
        if (r.last <= from) break;
        for (uint64_t n = r.first;; ++n) {
            const uint8_t *frame;
            size_t used;
            mm_journal_status st = lookup(j, n, &frame, &used);
            if (st != MM_JOURNAL_OK) return st;
            if (frame[3] == MM_KIND_LOSS) {
                mm_loss loss;
                if (mm_loss_decode(frame + MM_RECORD_HEADER_SIZE, used - MM_RECORD_HEADER_SIZE,
                                   n, &loss) != MM_OK) return MM_JOURNAL_CORRUPT;
                if (loss.kind == MM_LOSS_EVICTED && loss.first <= from && from <= loss.last) {
                    *seq = n;
                    return MM_JOURNAL_OK;
                }
            }
            if (n == r.last) break;
        }
    }
    return MM_JOURNAL_NOT_FOUND;
}

/* Validate an entire incoming group before publishing anything. */
static mm_journal_status inspect(mm_journal *j, const uint8_t *p, size_t n, seq_range *range,
                                 uint64_t *added_bytes)
{
    size_t off = 0;
    bool in_new_run = false;
    while (off < n) {
        mm_record rec;
        size_t used;
        if (mm_record_decode(p + off, n - off, &rec, &used) != MM_OK) return MM_JOURNAL_INVALID;
        if (off && (range->last == UINT64_MAX || rec.seq != range->last + 1))
            return MM_JOURNAL_INVALID;
        if (rec.kind == MM_KIND_LOSS) {
            mm_loss loss;
            if (mm_loss_decode(rec.payload, rec.len, rec.seq, &loss) != MM_OK) return MM_JOURNAL_INVALID;
        }
        if (off == 0) range->first = rec.seq;
        range->last = rec.seq;
        const uint8_t *old;
        size_t old_size;
        mm_journal_status st = lookup(j, rec.seq, &old, &old_size);
        if (st == MM_JOURNAL_NOT_FOUND) {
            if (!in_new_run) *added_bytes += 28u + j->config.stream.key_len;
            *added_bytes += used;
            in_new_run = true;
        } else {
            if (st != MM_JOURNAL_OK) return st;
            if (old_size != used || memcmp(old, p + off, used)) return MM_JOURNAL_CONFLICT;
            in_new_run = false;
        }
        off += used;
    }
    return MM_JOURNAL_OK;
}

static mm_journal_status remember(mm_journal *j, segment s, const uint8_t *records, size_t len)
{
    if (j->count == j->capacity) {
        void *p = grow(j->segments, &j->capacity, sizeof(*j->segments));
        if (!p) return MM_JOURNAL_NOMEM;
        j->segments = p;
    }
    mm_journal_status st = account(j, s.range);
    for (size_t off = 0; st == MM_JOURNAL_OK && off < len;) {
        const uint8_t *frame = records + off;
        uint32_t payload_size = mm_get32(frame + 4);
        if (frame[3] == MM_KIND_LOSS) {
            mm_loss loss;
            if (mm_loss_decode(frame + MM_RECORD_HEADER_SIZE, payload_size, mm_get64(frame + 8), &loss) != MM_OK)
                return MM_JOURNAL_CORRUPT;
            if (loss.kind == MM_LOSS_EVICTED) st = account(j, (seq_range){loss.first, loss.last});
        }
        off += MM_RECORD_HEADER_SIZE + (size_t)payload_size;
    }
    if (st != MM_JOURNAL_OK) return st;
    size_t pos = upper_bound(j, s.range.first);
    memmove(j->segments + pos + 1, j->segments + pos, (j->count - pos) * sizeof(*j->segments));
    j->segments[pos] = s;
    ++j->count;
    if (s.range.last > j->high) j->high = s.range.last;
    j->bytes += s.size;
    return MM_JOURNAL_OK;
}

static mm_journal_status restore_segment(mm_journal *j, seq_range range)
{
    size_t next = upper_bound(j, range.first);
    if ((next && j->segments[next-1].range.last >= range.first) ||
        (next < j->count && j->segments[next].range.first <= range.last)) return MM_JOURNAL_CORRUPT;
    uint8_t *buf;
    size_t size = 0, header = 0;
    mm_journal_status st = load_segment(j, range, &buf, &size);
    if (st != MM_JOURNAL_OK) return st;
    st = scan_segment(j, range, buf, size, &header);
    if (st == MM_JOURNAL_OK) {
        seq_range decoded = {0, 0};
        uint64_t added = 0;
        st = inspect(j, buf + header, size - header, &decoded, &added);
        if (st == MM_JOURNAL_CONFLICT || st == MM_JOURNAL_INVALID ||
            (st == MM_JOURNAL_OK && added != size)) st = MM_JOURNAL_CORRUPT;
    }
    if (st == MM_JOURNAL_OK) st = remember(j, (segment){range, size}, buf + header, size - header);
    free(buf);
    return st;
}

static mm_journal_status recover(mm_journal *j, bool provision)
{
    mm_journal_status st = load_state(j);
    bool new_store = st == MM_JOURNAL_NOT_FOUND;
    if (new_store && !provision) return st;
    if (!new_store && st != MM_JOURNAL_OK) return st;
    if (j->confirmed) {
        st = account(j, (seq_range){1, j->confirmed});
        if (st != MM_JOURNAL_OK) return st;
    }
    if (j->volume) {
        for (size_t i = 0; i < mm_volume_count(j->volume); ++i) {
            if (new_store) return MM_JOURNAL_CORRUPT;
            st = restore_segment(j, mm_volume_segment(j->volume, i).range);
            if (st != MM_JOURNAL_OK) return st;
        }
        if (j->config.mode == MM_JOURNAL_ORIGIN && mm_journal_prefix(j) != j->high)
            return MM_JOURNAL_CORRUPT;
        return new_store ? store_state(j, 0) : mm_volume_repair(j->volume);
    }
#ifndef ESP_PLATFORM
    /* fdopendir takes ownership of its fd. Opening "." also gives this scan
     * its own directory offset independent of the locked directory handle. */
    int fd = openat(j->dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return MM_JOURNAL_IO;
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return MM_JOURNAL_IO; }
    st = MM_JOURNAL_OK;
    bool pending = false;
    for (;;) {
        errno = 0;
        struct dirent *ent = readdir(dir);
        if (!ent) { if (errno) st = MM_JOURNAL_IO; break; }
        const char *name = ent->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, STATE_FILE)) continue;
        if (!strcmp(name, PENDING_FILE)) { pending = true; continue; }
        seq_range range;
        if (new_store || !parse_name(name, &range)) { st = MM_JOURNAL_CORRUPT; break; }
        st = restore_segment(j, range);
        if (st != MM_JOURNAL_OK) break;
    }
    if (closedir(dir) < 0 && st == MM_JOURNAL_OK) st = MM_JOURNAL_IO;
    if (st != MM_JOURNAL_OK) return st;
    if (j->config.mode == MM_JOURNAL_ORIGIN && mm_journal_prefix(j) != j->high)
        return MM_JOURNAL_CORRUPT;
    /* A staging file may contain a torn write. No published segment may do so. */
    if (pending && unlinkat(j->dir, PENDING_FILE, 0) < 0) return MM_JOURNAL_IO;
    if (new_store) return store_state(j, 0);
    return sync_fd(j, j->dir) < 0 ? MM_JOURNAL_IO : MM_JOURNAL_OK;
#else
    return MM_JOURNAL_INVALID;
#endif
}

static mm_journal_status open_store(mm_journal **out, const char *directory,
                                    const mm_journal_config *config, bool create,
                                    const mm_volume_options *options)
{
    if (!out) return MM_JOURNAL_INVALID;
    *out = NULL;
    if (!directory || !config || mm_stream_check(&config->stream) != MM_OK ||
        (config->mode != MM_JOURNAL_ORIGIN && config->mode != MM_JOURNAL_RECEIVER))
        return MM_JOURNAL_INVALID;
    const mm_receipt scope = {config->stream, config->role, config->issuer, config->destination, 0};
    uint8_t check[MM_RECEIPT_MAX];
    if (!mm_receipt_encode(check, sizeof(check), &scope)) return MM_JOURNAL_INVALID;

    mm_journal *j = calloc(1, sizeof(*j));
    if (!j) return MM_JOURNAL_NOMEM;
    j->config = *config;
    memcpy(j->key, config->stream.key, config->stream.key_len);
    j->config.stream.key = j->key;
    j->dir = -1;
    mm_journal_status st;
    if (options) st = mm_volume_open(&j->volume, directory, options, create);
#ifndef ESP_PLATFORM
    else {
        j->dir = open(directory, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        if (j->dir < 0) st = errno == ENOENT ? MM_JOURNAL_NOT_FOUND : MM_JOURNAL_IO;
        else if (flock(j->dir, LOCK_EX | LOCK_NB) < 0)
            st = errno == EWOULDBLOCK ? MM_JOURNAL_BUSY : MM_JOURNAL_IO;
        else st = MM_JOURNAL_OK;
    }
#else
    else st = MM_JOURNAL_INVALID;
#endif
    if (st == MM_JOURNAL_OK) st = recover(j, create);
    if (st != MM_JOURNAL_OK) { mm_journal_close(j); return st; }
    *out = j;
    return MM_JOURNAL_OK;
}

mm_journal_status mm_journal_open_volume(mm_journal **out, const char *path,
                                         const mm_journal_config *config,
                                         const mm_volume_options *options, bool create)
{
    if (!options) return MM_JOURNAL_INVALID;
    return open_store(out, path, config, create, options);
}

mm_journal_status mm_journal_open(mm_journal **out, const char *directory,
                                  const mm_journal_config *config, bool create)
{
#ifndef ESP_PLATFORM
    return open_store(out, directory, config, create, NULL);
#else
    if (!directory) return MM_JOURNAL_INVALID;
    char path[256];
    int n = snprintf(path, sizeof(path), "%s/PIPE.MM", directory);
    if (n < 0 || (size_t)n >= sizeof(path)) return MM_JOURNAL_INVALID;
    const mm_volume_options options = {16, 4096, MM_KEEP_OLD};
    return open_store(out, path, config, create, &options);
#endif
}

void mm_journal_close(mm_journal *j)
{
    if (!j) return;
    if (j->dir >= 0) close(j->dir);
    mm_volume_close(j->volume);
    free(j->segments);
    free(j->coverage);
    free(j->cache);
    free(j);
}

const mm_journal_config *mm_journal_config_get(const mm_journal *j) { return &j->config; }
bool mm_journal_needs_reopen(const mm_journal *j) { return j->uncertain; }
uint64_t mm_journal_prefix(const mm_journal *j)
{
    return j->ranges && j->coverage[0].first == 1 ? j->coverage[0].last : 0;
}
uint64_t mm_journal_missed(const mm_journal *j) { return j->volume ? mm_volume_missed(j->volume) : 0; }
uint64_t mm_journal_high(const mm_journal *j) { return j->high; }
uint64_t mm_journal_confirmed(const mm_journal *j) { return j->confirmed; }

static size_t segment_limit(mm_journal *j)
{
    return j->volume ? mm_volume_limit(j->volume) : MM_JOURNAL_SEGMENT_MAX;
}

static void forget_through(mm_journal *j, uint64_t through)
{
    size_t removed = 0;
    while (removed < j->count && j->segments[removed].range.last <= through)
        j->bytes -= j->segments[removed++].size;
    if (!removed) return;
    j->count -= removed;
    memmove(j->segments, j->segments + removed, j->count * sizeof(*j->segments));
    free(j->cache);
    j->cache = NULL;
}

static mm_journal_status publish_run(mm_journal *j, const uint8_t *records, size_t len,
                                     seq_range range, uint64_t remove_through, uint64_t missed)
{
    uint8_t header[MM_SEGMENT_HEADER_MAX];
    size_t h = mm_segment_header(header, sizeof(header), j->key, j->config.stream.key_len,
                                 range.first, mm_get32(records + 24));
    if (!h) return MM_JOURNAL_INVALID;
    mm_journal_status st;
    if (j->volume)
        st = mm_volume_publish(j->volume, range, header, h, records, len, remove_through, missed);
#ifndef ESP_PLATFORM
    else {
        char name[NAME_SIZE];
        segment_name(name, range);
        st = publish(j, name, header, h, records, len);
    }
#else
    else return MM_JOURNAL_INVALID;
#endif
    if (st == MM_JOURNAL_UNCERTAIN) return uncertain(j);
    if (st != MM_JOURNAL_OK) return st;
    /* If indexing runs out of memory, the data is already published. A reopen
     * reconstructs indexes before any further writes or receipts. */
    if (remove_through) forget_through(j, remove_through);
    st = remember(j, (segment){range, h + len}, records, len);
    return st == MM_JOURNAL_OK ? st : uncertain(j);
}

static mm_journal_status commit(mm_journal *j, const uint8_t *records, size_t len)
{
    size_t header = 28u + j->config.stream.key_len;
    if (len > segment_limit(j) - header) return MM_JOURNAL_TOO_SMALL;
    seq_range range = {0, 0};
    uint64_t added = 0;
    mm_journal_status st = inspect(j, records, len, &range, &added);
    if (st != MM_JOURNAL_OK || added == 0) return st;
    uint64_t max = j->config.max_bytes;
    if (j->bytes > UINT64_MAX - added || (max && (j->bytes >= max || added > max - j->bytes)))
        return MM_JOURNAL_FULL;

    /* A normal new batch publishes one segment. With a partially repeated
     * batch publish only disjoint missing runs. A retry skips any run already
     * published if a later run encounters an I/O failure. */
    size_t off = 0;
    while (off < len) {
        const uint8_t *rec = records + off;
        size_t start = off;
        off += MM_RECORD_HEADER_SIZE + (size_t)mm_get32(rec + 4);
        uint64_t first = mm_get64(rec + 8);
        if (containing(j, first)) continue;
        uint64_t last = first;
        while (off < len && !containing(j, last + 1)) {
            ++last; /* inspect checked that subsequent sequence numbers cannot wrap. */
            off += MM_RECORD_HEADER_SIZE + (size_t)mm_get32(records + off + 4);
        }
        st = publish_run(j, records + start, off - start, (seq_range){first, last}, 0, mm_journal_missed(j));
        if (st != MM_JOURNAL_OK) return st;
    }
    return MM_JOURNAL_OK;
}

mm_journal_status mm_journal_ingest(mm_journal *j, const uint8_t *records, size_t len)
{
    if (!j || j->config.mode != MM_JOURNAL_RECEIVER || (!records && len)) return MM_JOURNAL_INVALID;
    if (j->uncertain) return MM_JOURNAL_UNCERTAIN;
    return len ? commit(j, records, len) : MM_JOURNAL_OK;
}

#define LOSS_FRAME (MM_RECORD_HEADER_SIZE + MM_LOSS_SIZE)
static void encode_loss(uint8_t buf[LOSS_FRAME], mm_loss loss, uint64_t seq, uint64_t mono, uint32_t boot)
{
    mm_loss_encode(buf + MM_RECORD_HEADER_SIZE, &loss, seq);
    mm_record_header(buf, MM_KIND_LOSS, seq, mono, boot, buf + MM_RECORD_HEADER_SIZE, MM_LOSS_SIZE);
}

mm_journal_status mm_journal_flush_loss(mm_journal *j)
{
    if (!j || j->config.mode != MM_JOURNAL_ORIGIN) return MM_JOURNAL_INVALID;
    if (j->uncertain) return MM_JOURNAL_UNCERTAIN;
    uint64_t missed = mm_journal_missed(j);
    if (!missed) return MM_JOURNAL_OK;
    if (j->high == UINT64_MAX) return MM_JOURNAL_FULL;
    size_t bytes = 28u + j->config.stream.key_len + LOSS_FRAME;
    uint64_t max = j->config.max_bytes;
    if (max && (bytes > max || j->bytes > max - bytes)) return MM_JOURNAL_FULL;
    uint64_t seq = j->high + 1;
    uint8_t buf[LOSS_FRAME];
    encode_loss(buf, (mm_loss){MM_LOSS_MISSED, missed, 0}, seq, 0, 0);
    return publish_run(j, buf, sizeof(buf), (seq_range){seq, seq}, 0, 0);
}

/* The new data and its loss record share a spare slot. A single manifest
 * switch makes all three changes visible: new rows, evidence, retired slots.
 * Previous eviction records can be subsumed because each lost prefix begins
 * immediately above the durable receipt. Protect unconfirmed missed counts. */
static mm_journal_status replace_oldest(mm_journal *j, const mm_sample *rows, size_t count,
                                        uint8_t *records, size_t n)
{
    uint64_t bytes = j->bytes, through = 0;
    size_t kept = j->count, h = 28u + j->config.stream.key_len;
    uint64_t max = j->config.max_bytes;
    size_t incoming = h + n;
    size_t i = 0;
    while (kept >= mm_volume_capacity(j->volume) || (max && (incoming > max || bytes > max - incoming))) {
        if (i == j->count) return MM_JOURNAL_FULL;
        segment s = j->segments[i++];
        if (s.range.last > j->confirmed) {
            uint64_t seq = s.range.first > j->confirmed ? s.range.first : j->confirmed + 1;
            for (;;) {
                const uint8_t *frame;
                size_t used;
                mm_journal_status st = lookup(j, seq, &frame, &used);
                if (st != MM_JOURNAL_OK) return st;
                if (frame[3] == MM_KIND_LOSS && frame[MM_RECORD_HEADER_SIZE] == MM_LOSS_MISSED)
                    return MM_JOURNAL_FULL;
                if (seq == s.range.last) break;
                ++seq;
            }
            incoming = h + n + LOSS_FRAME;
        }
        through = s.range.last;
        bytes -= s.size;
        --kept;
    }
    if (!through) return MM_JOURNAL_FULL;
    uint64_t first = j->high + 1, last = j->high + count;
    if (through > j->confirmed) {
        if (last == UINT64_MAX) return MM_JOURNAL_FULL;
        ++last;
        encode_loss(records + n, (mm_loss){MM_LOSS_EVICTED, j->confirmed + 1, through},
                    last, rows[0].mono_us, rows[0].boot);
        n += LOSS_FRAME;
    }
    return publish_run(j, records, n, (seq_range){first, last}, through, mm_journal_missed(j));
}

mm_journal_status mm_journal_append_group(mm_journal *j, const mm_sample *rows, size_t count,
                                          uint64_t *first_seq)
{
    if (!first_seq) return MM_JOURNAL_INVALID;
    *first_seq = 0;
    if (!j || j->config.mode != MM_JOURNAL_ORIGIN || (!rows && count)) return MM_JOURNAL_INVALID;
    if (j->uncertain) return MM_JOURNAL_UNCERTAIN;
    if (!count) return MM_JOURNAL_OK;
    if (count > UINT64_MAX - j->high) return MM_JOURNAL_FULL;
    size_t len = 0;
    size_t cap = segment_limit(j) - 28u - j->config.stream.key_len;
    bool keep_new = j->volume && mm_volume_retention(j->volume) == MM_KEEP_NEW;
    if (keep_new) cap -= LOSS_FRAME;
    for (size_t i = 0; i < count; ++i) {
        if ((!rows[i].payload && rows[i].len) || rows[i].len > MM_RECORD_MAX_PAYLOAD)
            return MM_JOURNAL_INVALID;
        size_t n = MM_RECORD_HEADER_SIZE + (size_t)rows[i].len;
        if (n > cap - len) return MM_JOURNAL_TOO_SMALL;
        len += n;
    }
    uint8_t *buf = malloc(len + (keep_new ? LOSS_FRAME : 0));
    if (!buf) return MM_JOURNAL_NOMEM;
    mm_journal_status st = MM_JOURNAL_OK;
    size_t off = 0;
    uint64_t first = j->high + 1;
    for (size_t i = 0; i < count; ++i) {
        const mm_sample *row = rows + i;
        if (mm_record_header(buf + off, row->kind, first + i, row->mono_us, row->boot,
                             row->payload, row->len) != MM_OK) {
            st = MM_JOURNAL_INVALID;
            break;
        }
        off += MM_RECORD_HEADER_SIZE;
        if (row->len) memcpy(buf + off, row->payload, row->len);
        off += row->len;
    }
    if (st == MM_JOURNAL_OK) st = commit(j, buf, len);
    if (st == MM_JOURNAL_FULL && keep_new) st = replace_oldest(j, rows, count, buf, len);
    if (st == MM_JOURNAL_OK || st == MM_JOURNAL_UNCERTAIN) *first_seq = first;
    if (st == MM_JOURNAL_FULL && j->volume) {
        uint64_t observations = 0;
        for (size_t i = 0; i < count; ++i) observations += rows[i].kind == MM_KIND_DATA;
        uint64_t missed = mm_volume_missed(j->volume);
        if (observations > UINT64_MAX - missed) st = MM_JOURNAL_INVALID;
        else if (observations) {
            st = mm_volume_set_missed(j->volume, missed + observations);
            if (st == MM_JOURNAL_OK) st = MM_JOURNAL_FULL;
            else if (st == MM_JOURNAL_UNCERTAIN) uncertain(j);
        }
    }
    free(buf);
    return st;
}

mm_journal_status mm_journal_append(mm_journal *j, mm_kind kind, uint64_t mono_us, uint32_t boot,
                                    const void *payload, uint32_t len, uint64_t *seq)
{
    const mm_sample row = {kind, mono_us, boot, payload, len};
    return mm_journal_append_group(j, &row, 1, seq);
}

mm_journal_status mm_journal_confirm(mm_journal *j, const mm_receipt *receipt)
{
    if (!j || !receipt || j->config.mode != MM_JOURNAL_ORIGIN) return MM_JOURNAL_INVALID;
    if (j->uncertain) return MM_JOURNAL_UNCERTAIN;
    const mm_journal_config *c = &j->config;
    if (receipt->role != c->role || receipt->issuer != c->issuer ||
        receipt->destination != c->destination || !mm_stream_equal(&receipt->stream, &c->stream))
        return MM_JOURNAL_CONFLICT;
    if (receipt->through > j->high) return MM_JOURNAL_INVALID;
    if (receipt->through <= j->confirmed) return MM_JOURNAL_OK;
    mm_journal_status st = store_state(j, receipt->through);
    if (st == MM_JOURNAL_UNCERTAIN) return uncertain(j);
    if (st == MM_JOURNAL_OK) j->confirmed = receipt->through;
    return st;
}

mm_journal_status mm_journal_reclaim(mm_journal *j)
{
    if (!j || j->config.mode != MM_JOURNAL_ORIGIN) return MM_JOURNAL_INVALID;
    if (j->uncertain) return MM_JOURNAL_UNCERTAIN;
    if (j->volume) {
        mm_journal_status st = mm_volume_trim(j->volume, j->confirmed);
        if (st != MM_JOURNAL_OK) return st == MM_JOURNAL_UNCERTAIN ? uncertain(j) : st;
    }
    size_t kept = 0;
    for (size_t i = 0; i < j->count; ++i) {
        segment s = j->segments[i];
        if (s.range.last > j->confirmed) {
            j->segments[kept++] = s;
            continue;
        }
#ifndef ESP_PLATFORM
        if (!j->volume) {
            char name[NAME_SIZE];
            segment_name(name, s.range);
            if (unlinkat(j->dir, name, 0) < 0) return uncertain(j);
        }
#endif
        j->bytes -= s.size;
    }
    if (kept == j->count) {
        mm_journal_status st = mm_journal_flush_loss(j);
        return st == MM_JOURNAL_FULL ? MM_JOURNAL_OK : st;
    }
    j->count = kept;
    free(j->cache);
    j->cache = NULL;
#ifndef ESP_PLATFORM
    if (!j->volume && sync_fd(j, j->dir) < 0) return uncertain(j);
#endif
    mm_journal_status st = mm_journal_flush_loss(j);
    return st == MM_JOURNAL_FULL ? MM_JOURNAL_OK : st;
}

void mm_journal_set_barrier(mm_journal *j, mm_journal_barrier barrier, void *context)
{
    if (j->volume) mm_volume_set_barrier(j->volume, barrier, context);
    j->barrier = barrier;
    j->context = context;
}

const char *mm_journal_status_name(mm_journal_status st)
{
    static const char *const names[] = {
        "ok", "invalid", "io", "corrupt", "conflict", "not_found", "too_small",
        "full", "busy", "uncertain", "no_memory"
    };
    return (unsigned)st < sizeof(names) / sizeof(names[0]) ? names[st] : "?";
}
