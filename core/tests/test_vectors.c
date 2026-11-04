// SPDX-License-Identifier: Apache-2.0
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "micromeerschaum/mm.h"

#define MAX_FIELDS 16

static char line[1 << 18];
static uint8_t data[1 << 17];
static uint8_t scratch[1 << 17];

static int hex_nibble(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static size_t decode_hex(const char *hex, uint8_t *out, size_t cap)
{
    if (strcmp(hex, "-") == 0) return 0;
    size_t n = strlen(hex);
    if (n % 2 != 0 || n / 2 > cap) {
        fprintf(stderr, "bad hex field: %.40s\n", hex);
        exit(2);
    }
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            fprintf(stderr, "bad hex field: %.40s\n", hex);
            exit(2);
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n / 2;
}

static uint64_t num(const char *field)
{
    return strtoull(field, NULL, 10);
}

static int fail(const char *file, const char *name, const char *what)
{
    fprintf(stderr, "%s %s: %s\n", file, name, what);
    return 1;
}

/* Each check gets the fields of one vector line and returns its failure count. */
typedef int (*check_fn)(char **f, int n);

static int run(const char *file, int min_fields, check_fn check)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", MM_VECTORS_DIR, file);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror(path);
        exit(2);
    }
    int cases = 0, failures = 0;
    while (fgets(line, sizeof line, fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] != '\n' && !feof(fp)) {
            fprintf(stderr, "%s: line too long\n", file);
            exit(2);
        }
        if (line[0] == '#' || line[0] == '\n') continue;
        char *f[MAX_FIELDS];
        int n = 0;
        for (char *t = strtok(line, " \n"); t && n < MAX_FIELDS; t = strtok(NULL, " \n")) f[n++] = t;
        if (n < min_fields) {
            fprintf(stderr, "%s: short vector line\n", file);
            exit(2);
        }
        failures += check(f, n);
        cases++;
    }
    fclose(fp);
    printf("%s: %d vectors, %d failures\n", file, cases, failures);
    return cases == 0 || failures != 0;
}

static int check_crc32(char **f, int n)
{
    (void)n;
    size_t len = decode_hex(f[0], data, sizeof data);
    uint32_t want = (uint32_t)strtoul(f[1], NULL, 16);
    uint32_t whole = mm_crc32(0, data, len);
    uint32_t split = mm_crc32(mm_crc32(0, data, len / 2), data + len / 2, len - len / 2);
    return (whole != want || split != want) ? fail("crc32.txt", f[0], "crc mismatch") : 0;
}

static int check_record(char **f, int n)
{
    size_t len = decode_hex(f[1], data, sizeof data);
    mm_record rec;
    size_t used = 0;
    mm_status st = mm_record_decode(data, len, &rec, &used);
    if (strcmp(mm_status_name(st), f[2]) != 0) return fail("records.txt", f[0], mm_status_name(st));
    if (st != MM_OK) return 0;
    if (n < 9) return fail("records.txt", f[0], "short ok line");
    if (used != num(f[3]) || (uint64_t)rec.kind != num(f[4]) || rec.seq != num(f[5]) ||
        rec.mono_us != num(f[6]) || rec.boot != num(f[7]) || rec.len != num(f[8]) ||
        rec.payload != data + MM_RECORD_HEADER_SIZE) {
        return fail("records.txt", f[0], "decoded fields differ");
    }
    uint8_t header[MM_RECORD_HEADER_SIZE];
    if (mm_record_header(header, rec.kind, rec.seq, rec.mono_us, rec.boot, rec.payload, rec.len) != MM_OK ||
        memcmp(header, data, sizeof header) != 0) {
        return fail("records.txt", f[0], "re-encoded header differs");
    }
    return 0;
}

static int check_segment(char **f, int n)
{
    size_t len = decode_hex(f[1], data, sizeof data);
    mm_scan s;
    mm_status st = mm_segment_scan(data, len, &s);
    if (strcmp(mm_status_name(st), f[2]) != 0) return fail("segments.txt", f[0], mm_status_name(st));
    if (st != MM_OK) return 0;
    if (n < 11) return fail("segments.txt", f[0], "short ok line");
    uint8_t key[MM_KEY_MAX];
    size_t key_len = decode_hex(f[4], key, sizeof key);
    if (s.header_size != num(f[3]) || s.key_len != key_len || memcmp(s.key, key, key_len) != 0 ||
        s.first_seq != num(f[5]) || s.boot != num(f[6]) || s.records != num(f[7]) ||
        s.last_seq != num(f[8]) || s.frontier != num(f[9]) || strcmp(mm_status_name(s.stop), f[10]) != 0) {
        return fail("segments.txt", f[0], "scan differs");
    }
    size_t size = mm_segment_header(scratch, sizeof scratch, s.key, s.key_len, s.first_seq, s.boot);
    if (size != s.header_size || memcmp(scratch, data, size) != 0) {
        return fail("segments.txt", f[0], "re-encoded header differs");
    }
    return 0;
}

static int hops_match(const mm_batch *b, const char *want)
{
    char text[MM_BATCH_MAX_HOPS * 64 + 2] = "-";
    size_t at = 0;
    for (size_t i = 0; i < b->hop_count; i++) {
        at += (size_t)snprintf(text + at, sizeof text - at, "%s%" PRIu64 ":%" PRId64 ":%d", i ? "," : "",
                               b->hops[i].node, b->hops[i].received_us, (int)b->hops[i].transport);
    }
    return strcmp(text, want) == 0;
}

static int check_batch(char **f, int n)
{
    size_t len = decode_hex(f[1], data, sizeof data);
    mm_batch b;
    uint64_t first = 0, last = 0;
    mm_status st = mm_batch_validate(data, len, &b, &first, &last);
    if (strcmp(mm_status_name(st), f[2]) != 0) return fail("batches.txt", f[0], mm_status_name(st));
    if (st != MM_OK) return 0;
    if (n < 10) return fail("batches.txt", f[0], "short ok line");
    mm_batch h;
    size_t header_size = 0;
    uint8_t key[MM_KEY_MAX];
    size_t key_len = decode_hex(f[5], key, sizeof key);
    if (mm_batch_header_decode(data, len, &h, &header_size) != MM_OK || header_size != num(f[3]) ||
        b.origin != num(f[4]) || b.key_len != key_len || memcmp(b.key, key, key_len) != 0 ||
        !hops_match(&b, f[6]) || b.count != num(f[7]) || first != num(f[8]) || last != num(f[9])) {
        return fail("batches.txt", f[0], "batch differs");
    }
    size_t size = mm_batch_header(scratch, sizeof scratch, &b);
    if (size != header_size || memcmp(scratch, data, size) != 0) {
        return fail("batches.txt", f[0], "re-encoded header differs");
    }
    return 0;
}

int main(void)
{
    int failed = 0;
    failed |= run("crc32.txt", 2, check_crc32);
    failed |= run("records.txt", 3, check_record);
    failed |= run("segments.txt", 3, check_segment);
    failed |= run("batches.txt", 3, check_batch);
    return failed;
}
