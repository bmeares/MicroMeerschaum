// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "micromeerschaum/document.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void payload_boundary(void)
{
    static char text[MM_RECORD_MAX_PAYLOAD];
    static uint8_t payload[MM_RECORD_MAX_PAYLOAD];
    // A one-character name and text tag take thirteen framing bytes.
    mm_field field = {{"x", 1}, {.type = MM_VALUE_TEXT,
                                 .as.text = {text, MM_RECORD_MAX_PAYLOAD - 13}}};
    mm_row row = {&field, 1};
    size_t used, count = 1;
    CHECK(mm_document_encode(payload, sizeof payload, row, &used) == MM_OK);
    CHECK(used == MM_RECORD_MAX_PAYLOAD);
    mm_field decoded;
    CHECK(mm_document_decode(payload, used, &decoded, 1, &count) == MM_OK && count == 1);
    CHECK(decoded.value.as.text.len == MM_RECORD_MAX_PAYLOAD - 13);
    ++field.value.as.text.len;
    CHECK(mm_document_encode(payload, sizeof payload, row, &used) == MM_BAD_LENGTH && used == 0);
    CHECK(mm_document_decode(payload, sizeof payload, NULL, 0, &count) == MM_BAD_LENGTH && count == 0);
    count = 1;
    CHECK(mm_document_decode(NULL, 2, &decoded, 1, &count) == MM_BAD_FIELD && count == 0);
}

int main(void)
{
    payload_boundary();
    FILE *input = fopen(MM_VECTORS_DIR "/documents-v1.txt", "r");
    CHECK(input);
    char line[512], name[32], hex[384], expected[24];
    size_t seen = 0;
    while (fgets(line, sizeof line, input)) {
        if (line[0] == '#') continue;
        CHECK(sscanf(line, "%31s %383s %23s", name, hex, expected) == 3);
        uint8_t raw[192], encoded[192];
        size_t len = strlen(hex) / 2;
        for (size_t i = 0; i < len; ++i) {
            unsigned byte;
            CHECK(sscanf(hex + 2 * i, "%2x", &byte) == 1);
            raw[i] = (uint8_t)byte;
        }
        mm_field fields[MM_DOCUMENT_MAX_FIELDS];
        size_t count, used;
        mm_status status = mm_document_decode(raw, len, fields, MM_DOCUMENT_MAX_FIELDS, &count);
        CHECK(strcmp(mm_status_name(status), expected) == 0);
        if (status != MM_OK) continue;
        if (strcmp(name, "scalars") == 0) {
            CHECK(count == 7 && fields[0].value.type == MM_VALUE_NULL);
            CHECK(fields[1].value.type == MM_VALUE_BOOL && !fields[1].value.as.boolean);
            CHECK(fields[2].value.type == MM_VALUE_BOOL && fields[2].value.as.boolean);
            CHECK(fields[3].value.type == MM_VALUE_INT && fields[3].value.as.integer == INT64_MIN);
            CHECK(fields[4].value.type == MM_VALUE_FLOAT && signbit(fields[4].value.as.real));
            CHECK(fields[5].value.type == MM_VALUE_TEXT && fields[5].value.as.text.len == 4);
            CHECK(memcmp(fields[5].value.as.text.data, "a\0\xc3\xa9", 4) == 0);
            CHECK(fields[6].value.type == MM_VALUE_TIME && fields[6].value.as.timestamp_us == -1);
        }
        mm_row row = {fields, count};
        CHECK(mm_document_encode(NULL, 0, row, &used) == MM_OK && used == len);
        CHECK(mm_document_encode(encoded, len - 1, row, &used) == MM_BAD_LENGTH && used == len);
        CHECK(mm_document_encode(encoded, len, row, &used) == MM_OK && used == len);
        CHECK(memcmp(encoded, raw, len) == 0);
        ++seen;
    }
    CHECK(fclose(input) == 0 && seen == 3);
}
