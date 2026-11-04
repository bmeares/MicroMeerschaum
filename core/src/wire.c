// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/mm.h"

const char *mm_status_name(mm_status status)
{
    switch (status) {
    case MM_OK: return "ok";
    case MM_END: return "end";
    case MM_TRUNCATED: return "truncated";
    case MM_BAD_MAGIC: return "bad_magic";
    case MM_BAD_VERSION: return "bad_version";
    case MM_BAD_KIND: return "bad_kind";
    case MM_BAD_LENGTH: return "bad_length";
    case MM_BAD_SEQ: return "bad_seq";
    case MM_SEQ_GAP: return "seq_gap";
    case MM_BAD_KEY: return "bad_key";
    case MM_BAD_FIELD: return "bad_field";
    case MM_BAD_CRC: return "bad_crc";
    case MM_TRAILING: return "trailing";
    }
    return "?";
}

/* Strict UTF-8 (RFC 3629): no overlongs, surrogates or code points past U+10FFFF. */
static int utf8_valid(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t width;
        uint8_t lo = 0x80, hi = 0xBF;
        if (c < 0x80) {
            i++;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            width = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            width = 3;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            width = 4;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        } else {
            return 0;
        }
        if (n - i < width || s[i + 1] < lo || s[i + 1] > hi) return 0;
        for (size_t k = 2; k < width; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return 0;
        }
        i += width;
    }
    return 1;
}

mm_status mm_key_check(const char *key, size_t len)
{
    const uint8_t *s = (const uint8_t *)key;
    size_t seps = 0, at[2] = {0, 0};
    if (len < 1 || len > MM_KEY_MAX || !utf8_valid(s, len)) return MM_BAD_KEY;
    for (size_t i = 0; i < len; i++) {
        if (s[i] != 0x1F) continue;
        if (seps == 2) return MM_BAD_KEY;
        at[seps++] = i;
    }
    if (seps != 2 || at[0] == 0 || at[1] == at[0] + 1) return MM_BAD_KEY;
    return MM_OK;
}
