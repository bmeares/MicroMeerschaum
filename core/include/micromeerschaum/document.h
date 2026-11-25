// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_DOCUMENT_H
#define MICROMEERSCHAUM_DOCUMENT_H

#include <stdbool.h>
#include "micromeerschaum/mm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MM_DOCUMENT_MAX_FIELDS 64u

typedef struct { const char *data; size_t len; } mm_text;
typedef enum {
    MM_VALUE_NULL, MM_VALUE_BOOL, MM_VALUE_INT, MM_VALUE_FLOAT, MM_VALUE_TEXT, MM_VALUE_TIME
} mm_value_type;
typedef struct {
    mm_value_type type;
    union {
        bool boolean;
        int64_t integer;
        double real;
        mm_text text;
        int64_t timestamp_us; /* Signed Unix microseconds in UTC. */
    } as;
} mm_value;

typedef struct { mm_text name; mm_value value; } mm_field;
typedef struct { const mm_field *fields; size_t count; } mm_row;

/* Validate and measure when out is NULL. On insufficient capacity return MM_BAD_LENGTH
 * and set used to the required size. Writes only after validating the entire row.
 * Output and input memory must be disjoint. */
mm_status mm_document_encode(uint8_t *out, size_t cap, mm_row row, size_t *used);

/* Validate exactly one payload. Fields borrow their name and text slices from buf.
 * On failure count is zero and fields are unspecified. Requires capacity >= wire count. */
mm_status mm_document_decode(const uint8_t *buf, size_t len, mm_field *fields,
                             size_t capacity, size_t *count);

#ifdef __cplusplus
}
#endif
#endif
