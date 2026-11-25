// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/protocol.h"
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; \
} } while (0)

static int check_vectors(void)
{
    FILE *file = fopen(MM_VECTORS_DIR "/protocol-v1.txt", "r");
    CHECK(file != NULL);
    char line[2048];
    size_t cases = 0;
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#') continue;
        char kind[16], name[64], hex[1536], expected[32];
        CHECK(sscanf(line, "%15s %63s %1535s %31s", kind, name, hex, expected) == 4);
        size_t len = strlen(hex) / 2;
        uint8_t bytes[768], encoded[MM_V1_BATCH_HEADER_MAX];
        CHECK(strlen(hex) == 2 * len && len <= sizeof(bytes));
        for (size_t i = 0; i < len; i++) {
            unsigned value;
            CHECK(sscanf(hex + i * 2, "%2x", &value) == 1);
            bytes[i] = (uint8_t)value;
        }
        mm_status status;
        size_t used = 0, rebuilt = 0;
        if (strcmp(kind, "stream") == 0) {
            mm_stream stream;
            status = mm_stream_decode(bytes, len, &stream);
            if (status == MM_OK) {
                used = len;
                rebuilt = mm_stream_encode(encoded, sizeof(encoded), &stream);
                CHECK(stream.origin == UINT64_C(0x0123456789abcdef));
                CHECK(stream.generation == (strcmp(name, "new_generation") == 0
                                            ? 2 : UINT64_C(0xfedcba9876543210)));
            }
        } else if (strcmp(kind, "receipt") == 0) {
            mm_receipt receipt;
            status = mm_receipt_decode(bytes, len, &receipt);
            if (status == MM_OK) {
                used = len;
                rebuilt = mm_receipt_encode(encoded, sizeof(encoded), &receipt);
                if (strcmp(name, "custody") == 0) {
                    CHECK(receipt.role == MM_CUSTODY && receipt.issuer == 17);
                    CHECK(receipt.destination == 4321 && receipt.through == 6);
                }
            }
        } else if (strcmp(kind, "batch") == 0) {
            mm_v1_batch batch;
            uint64_t first, last;
            status = mm_v1_batch_validate(bytes, len, &batch, &used, &first, &last);
            if (status == MM_OK) {
                rebuilt = mm_v1_batch_header(encoded, sizeof(encoded), &batch);
                if (strcmp(name, "two_equal_readings") == 0) {
                    CHECK(batch.destination == 4321 && batch.count == 2 && first == 42 && last == 43);
                    CHECK(batch.hop_count == 1 && batch.hops[0].received_us == -99);
                }
                if (batch.count == 0) CHECK(first == 0 && last == 0);
            }
        } else {
            CHECK(strcmp(kind, "loss") == 0);
            mm_loss loss;
            status = mm_loss_decode(bytes, len, 8, &loss);
            if (status == MM_OK) {
                used = MM_LOSS_SIZE;
                CHECK(mm_loss_encode(encoded, &loss, 8) == MM_OK);
                rebuilt = MM_LOSS_SIZE;
            }
        }
        if (strcmp(mm_status_name(status), expected) != 0) {
            fprintf(stderr, "%s %s: wanted %s, got %s\n", kind, name, expected, mm_status_name(status));
            return 1;
        }
        if (status == MM_OK) CHECK(rebuilt == used && used > 0 && !memcmp(bytes, encoded, used));
        cases++;
    }
    CHECK(!ferror(file) && fclose(file) == 0 && cases > 0);
    return 0;
}

static int check_boundaries(void)
{
    const char key[] = "a\x1f" "b\x1f";
    mm_stream stream = {.origin = 0, .generation = 1, .key = key, .key_len = sizeof(key) - 1};
    uint8_t scratch[MM_V1_BATCH_HEADER_MAX];
    memset(scratch, 0xaa, sizeof(scratch));
    mm_v1_batch batch = {.stream = stream, .count = 0};
    size_t len = mm_v1_batch_header(scratch, 1, &batch);
    CHECK(len == 0 && scratch[0] == 0xaa);
    batch.hop_count = 1;
    batch.hops[0].transport = (mm_transport)-1;
    CHECK(mm_v1_batch_header(scratch, sizeof(scratch), &batch) == 0 && scratch[0] == 0xaa);
    mm_receipt receipt = {.stream = stream, .role = MM_DESTINATION, .issuer = 2, .destination = 3};
    CHECK(mm_receipt_encode(scratch, sizeof(scratch), &receipt) == 0 && scratch[0] == 0xaa);
    receipt.issuer = receipt.destination;
    len = mm_receipt_encode(scratch, sizeof(scratch), &receipt);
    CHECK(len == MM_RECEIPT_FIXED_SIZE + sizeof(key) - 1);
    for (size_t i = 0; i < len; i++) CHECK(mm_receipt_decode(scratch, i, &receipt) == MM_TRUNCATED);
    CHECK(mm_receipt_decode(scratch, len, &receipt) == MM_OK && mm_stream_equal(&stream, &receipt.stream));
    stream.generation++;
    CHECK(!mm_stream_equal(&stream, &receipt.stream));
    mm_loss loss = {MM_LOSS_MISSED, 1, 0};
    CHECK(mm_loss_encode(scratch, &loss, 0) == MM_BAD_SEQ);
    return 0;
}

int main(void)
{
    return check_vectors() || check_boundaries();
}
