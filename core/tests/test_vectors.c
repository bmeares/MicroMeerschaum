// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "micromeerschaum/mm.h"

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
        fprintf(stderr, "bad hex field: %s\n", hex);
        exit(2);
    }
    for (size_t i = 0; i < n / 2; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            fprintf(stderr, "bad hex field: %s\n", hex);
            exit(2);
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return n / 2;
}

int main(void)
{
    FILE *f = fopen(MM_VECTORS_DIR "/crc32.txt", "r");
    if (!f) {
        perror(MM_VECTORS_DIR "/crc32.txt");
        return 2;
    }
    static char line[4096];
    static char hex[4096];
    static uint8_t buf[2048];
    int cases = 0, failures = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned long want = 0;
        if (sscanf(line, "%4095s %lx", hex, &want) != 2) {
            fprintf(stderr, "bad vector line: %s", line);
            return 2;
        }
        size_t len = decode_hex(hex, buf, sizeof buf);
        uint32_t whole = mm_crc32(0, buf, len);
        uint32_t split = mm_crc32(mm_crc32(0, buf, len / 2), buf + len / 2, len - len / 2);
        if (whole != (uint32_t)want || split != (uint32_t)want) {
            fprintf(stderr, "crc32 %s: want %08lx got %08x (split %08x)\n",
                    hex, want, (unsigned)whole, (unsigned)split);
            failures++;
        }
        cases++;
    }
    fclose(f);
    printf("crc32: %d vectors, %d failures\n", cases, failures);
    return (cases > 0 && failures == 0) ? 0 : 1;
}
