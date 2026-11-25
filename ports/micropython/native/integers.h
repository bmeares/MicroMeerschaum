// SPDX-License-Identifier: Apache-2.0
#ifndef MM_MICROPYTHON_INTEGERS_H
#define MM_MICROPYTHON_INTEGERS_H

#include "py/runtime.h"
#include "py/objint.h"

static inline uint64_t integer_bits(mp_obj_t value, bool is_signed) {
    if (!mp_obj_is_int(value)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected integer"));
    }
    bool negative = mp_obj_int_sign(value) < 0;
    if (!is_signed && negative) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected unsigned integer"));
    }
    if (mp_obj_is_small_int(value)) {
        return (uint64_t)MP_OBJ_SMALL_INT_VALUE(value);
    }
    byte le[8];
    if (!mp_obj_int_to_bytes_impl(value, false, sizeof le, le)
        || (is_signed && negative != ((le[7] & 128) != 0))) {
        mp_raise_ValueError(MP_ERROR_TEXT("integer outside 64-bit range"));
    }
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) {
        n = (n << 8) | le[i];
    }
    return n;
}

static inline uint32_t bounded(mp_obj_t value, uint32_t max) {
    uint64_t n = integer_bits(value, false);
    if (n > max) {
        mp_raise_ValueError(MP_ERROR_TEXT("value exceeds limit"));
    }
    return (uint32_t)n;
}

#endif
