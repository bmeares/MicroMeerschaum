// SPDX-License-Identifier: Apache-2.0
#include <LittleFS.h>
#include <MicroMeerschaum.h>

// Use a formatted LittleFS partition. Choose a distinct origin for each node
// and persist its generation across reboot when provisioning a deployment.
void setup() {
    Serial.begin(115200);
    if (!LittleFS.begin(false)) {
        Serial.println("Mount a formatted LittleFS partition to run LocalPipe.");
        return;
    }
    char key[MM_KEY_MAX];
    size_t key_len;
    if (mm_pipe_key(key, sizeof key, &key_len, "sensor", "temperature", nullptr) != MM_OK)
        return;
    const mm_journal_config config = {{1, 1, key, key_len}, MM_JOURNAL_ORIGIN,
                                      MM_DESTINATION, 2, 2, 0};
    const mm_volume_options volume = {4, 2048, MM_KEEP_OLD};
    mm_journal *journal = nullptr;
    auto status = mm_journal_open_volume(&journal, "/littlefs/readings.mm", &config, &volume, true);
    if (status == MM_JOURNAL_OK) {
        uint8_t scratch[128];
        mm::Pipe pipe;
        status = pipe.open("sensor", "temperature", nullptr, mm_pipe_journal_target(journal),
                           scratch, sizeof scratch);
        if (status == MM_JOURNAL_OK) {
            const mm_field fields[] = {mm::field("celsius", 21.5)};
            const mm_row rows[] = {mm::row(fields)};
            auto result = pipe.sync(rows);
            status = result.status;
            Serial.printf("Committed %u row(s), highest sequence %llu\n",
                          (unsigned)result.confirmed, (unsigned long long)mm_journal_high(journal));
        }
        mm_journal_close(journal);
    }
    Serial.println(mm_journal_status_name(status));
}

void loop() { delay(1000); }
