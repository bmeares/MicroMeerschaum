// SPDX-License-Identifier: Apache-2.0
#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "micromeerschaum/mm.h"

extern "C" void app_main(void)
{
    // One sensor reading, 21.4 degrees Celsius in signed little-endian tenths.
    constexpr uint8_t sample[] = {0xd6, 0x00};
    uint8_t bytes[MM_RECORD_HEADER_SIZE + sizeof(sample)];
    auto *payload = bytes + MM_RECORD_HEADER_SIZE;
    std::memcpy(payload, sample, sizeof(sample));

    mm_status status = mm_record_header(bytes, MM_KIND_DATA, 1, 1000, 1, payload, sizeof(sample));
    if (status != MM_OK) {
        ESP_LOGE("codec", "Encoding failed: %s", mm_status_name(status));
        return;
    }

    mm_record record{};
    size_t consumed = 0;
    status = mm_record_decode(bytes, sizeof(bytes), &record, &consumed);
    if (status != MM_OK) {
        ESP_LOGE("codec", "Decoding failed: %s", mm_status_name(status));
        return;
    }
    ESP_LOGI("codec", "Decoded record %" PRIu64 " (%zu bytes)", record.seq, consumed);
}
