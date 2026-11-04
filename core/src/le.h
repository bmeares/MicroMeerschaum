// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_LE_H
#define MICROMEERSCHAUM_LE_H

#include <stddef.h>
#include <stdint.h>

static inline uint32_t mm_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t mm_get64(const uint8_t *p)
{
    return (uint64_t)mm_get32(p) | (uint64_t)mm_get32(p + 4) << 32;
}

static inline int64_t mm_get_i64(const uint8_t *p)
{
    uint64_t v = mm_get64(p);
    return v <= (uint64_t)INT64_MAX ? (int64_t)v : -(int64_t)~v - 1;
}

static inline void mm_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void mm_put64(uint8_t *p, uint64_t v)
{
    mm_put32(p, (uint32_t)v);
    mm_put32(p + 4, (uint32_t)(v >> 32));
}

static inline int mm_all_zero(const uint8_t *p, size_t n)
{
    while (n--) {
        if (*p++) return 0;
    }
    return 1;
}

#endif
