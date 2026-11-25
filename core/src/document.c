// SPDX-License-Identifier: Apache-2.0
#include <float.h>
#include <string.h>
#include "micromeerschaum/document.h"
#include "le.h"
#include "utf8.h"

_Static_assert(sizeof(double) == sizeof(uint64_t) && FLT_RADIX == 2 &&
               DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024, "document floats require binary64");

enum { TAG_NULL, TAG_FALSE, TAG_TRUE, TAG_INT, TAG_FLOAT, TAG_TEXT, TAG_TIME };
static const uint8_t magic[] = {'M','M','D','C'};

static bool valid_name(mm_text name, const mm_field *previous, size_t count)
{
    if (!name.data || !name.len || name.len > UINT8_MAX || memchr(name.data, 0, name.len) ||
        !mm_utf8_valid((const uint8_t *)name.data, name.len)) return false;
    for (size_t i = 0; i < count; ++i) {
        mm_text other = previous[i].name;
        if (name.len == other.len && !memcmp(name.data, other.data, name.len)) return false;
    }
    return true;
}

static bool finite_bits(uint64_t bits)
{
    return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
}

/* Shared measurement / write path. The write pass receives a validated row and capacity. */
static mm_status put_value(uint8_t *out, mm_value value, size_t *size)
{
    uint64_t bits = 0;
    uint8_t tag;
    *size = 1;
    switch (value.type) {
    case MM_VALUE_NULL: tag = TAG_NULL; break;
    case MM_VALUE_BOOL: tag = value.as.boolean ? TAG_TRUE : TAG_FALSE; break;
    case MM_VALUE_INT: tag = TAG_INT; bits = (uint64_t)value.as.integer; break;
    case MM_VALUE_TIME: tag = TAG_TIME; bits = (uint64_t)value.as.timestamp_us; break;
    case MM_VALUE_FLOAT:
        tag = TAG_FLOAT;
        memcpy(&bits, &value.as.real, sizeof bits);
        if (!finite_bits(bits)) return MM_BAD_FIELD;
        break;
    case MM_VALUE_TEXT: {
        tag = TAG_TEXT;
        mm_text text = value.as.text;
        if (text.len > MM_RECORD_MAX_PAYLOAD) return MM_BAD_LENGTH;
        if ((!text.data && text.len) || !mm_utf8_valid((const uint8_t *)text.data, text.len))
            return MM_BAD_FIELD;
        *size += 4 + text.len;
        if (out) {
            mm_put32(out + 1, (uint32_t)text.len);
            if (text.len) memcpy(out + 5, text.data, text.len);
        }
        break;
    }
    default: return MM_BAD_FIELD;
    }
    if (tag == TAG_INT || tag == TAG_FLOAT || tag == TAG_TIME) {
        *size += 8;
        if (out) mm_put64(out + 1, bits);
    }
    if (out) *out = tag;
    return MM_OK;
}

mm_status mm_document_encode(uint8_t *out, size_t cap, mm_row row, size_t *used)
{
    if (!used) return MM_BAD_FIELD;
    *used = 0;
    if (row.count > MM_DOCUMENT_MAX_FIELDS) return MM_BAD_LENGTH;
    if (row.count && !row.fields) return MM_BAD_FIELD;
    size_t len = 6;
    for (size_t i = 0; i < row.count; ++i) {
        const mm_field *f = row.fields + i;
        if (!valid_name(f->name, row.fields, i)) return MM_BAD_FIELD;
        size_t size;
        mm_status st = put_value(NULL, f->value, &size);
        if (st != MM_OK) return st;
        len += 1 + f->name.len + size;
        if (len > MM_RECORD_MAX_PAYLOAD) return MM_BAD_LENGTH;
    }
    *used = len;
    if (!out) return MM_OK;
    if (cap < len) return MM_BAD_LENGTH;
    memcpy(out, magic, sizeof magic);
    out[4] = 1;
    out[5] = (uint8_t)row.count;
    size_t at = 6;
    for (size_t i = 0; i < row.count; ++i) {
        const mm_field *f = row.fields + i;
        out[at++] = (uint8_t)f->name.len;
        memcpy(out + at, f->name.data, f->name.len);
        at += f->name.len;
        size_t size;
        (void)put_value(out + at, f->value, &size);
        at += size;
    }
    return MM_OK;
}

mm_status mm_document_decode(const uint8_t *buf, size_t len, mm_field *fields,
                             size_t capacity, size_t *count)
{
    if (!count) return MM_BAD_FIELD;
    *count = 0;
    if (!buf && len) return MM_BAD_FIELD;
    if (len > MM_RECORD_MAX_PAYLOAD) return MM_BAD_LENGTH;
    if (len < 6) return MM_TRUNCATED;
    if (memcmp(buf, magic, sizeof magic)) return MM_BAD_MAGIC;
    if (buf[4] != 1) return MM_BAD_VERSION;
    size_t n = buf[5];
    if (n > MM_DOCUMENT_MAX_FIELDS || n > capacity) return MM_BAD_LENGTH;
    if (n && !fields) return MM_BAD_FIELD;
    size_t at = 6;
    for (size_t i = 0; i < n; ++i) {
        if (at == len) return MM_TRUNCATED;
        size_t size = buf[at++];
        if (size > len - at) return MM_TRUNCATED;
        mm_field *field = fields + i;
        field->name = (mm_text){(const char *)buf + at, size};
        if (!valid_name(field->name, fields, i)) return MM_BAD_FIELD;
        at += size;
        if (at == len) return MM_TRUNCATED;
        uint8_t tag = buf[at++];
        mm_value *v = &field->value;
        switch (tag) {
        case TAG_NULL: v->type = MM_VALUE_NULL; break;
        case TAG_FALSE: case TAG_TRUE:
            v->type = MM_VALUE_BOOL;
            v->as.boolean = tag == TAG_TRUE;
            break;
        case TAG_INT: case TAG_TIME: case TAG_FLOAT:
            if (len - at < 8) return MM_TRUNCATED;
            if (tag == TAG_INT) {
                v->type = MM_VALUE_INT;
                v->as.integer = mm_get_i64(buf + at);
            } else if (tag == TAG_TIME) {
                v->type = MM_VALUE_TIME;
                v->as.timestamp_us = mm_get_i64(buf + at);
            } else {
                uint64_t bits = mm_get64(buf + at);
                if (!finite_bits(bits)) return MM_BAD_FIELD;
                v->type = MM_VALUE_FLOAT;
                memcpy(&v->as.real, &bits, sizeof bits);
            }
            at += 8;
            break;
        case TAG_TEXT:
            if (len - at < 4) return MM_TRUNCATED;
            size = mm_get32(buf + at);
            at += 4;
            if (size > len - at) return MM_TRUNCATED;
            if (!mm_utf8_valid(buf + at, size)) return MM_BAD_FIELD;
            v->type = MM_VALUE_TEXT;
            v->as.text = (mm_text){(const char *)buf + at, size};
            at += size;
            break;
        default: return MM_BAD_FIELD;
        }
    }
    if (at != len) return MM_TRAILING;
    *count = n;
    return MM_OK;
}
