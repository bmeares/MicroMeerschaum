// SPDX-License-Identifier: Apache-2.0
#include <LittleFS.h>
#include <MicroMeerschaum.h>

// Two independent node stores on the same formatted volume let a single
// development board exercise a lost receipt. Use authenticated GATT or HTTPS
// to carry these same batch and receipt bytes between separate machines.
static mm_journal_status exercise(mm_journal *origin, mm_journal *peer) {
    uint64_t seq;
    const char reading[] = "sample";
    auto st = mm_journal_append(origin, MM_KIND_DATA, micros(), 1, reading, sizeof(reading) - 1, &seq);
    if (st != MM_JOURNAL_OK) return st;
    uint8_t batch[512], receipt[MM_RECEIPT_MAX];
    size_t count, size;
    st = mm_peer_batch(origin, mm_journal_confirmed(origin), 8, batch, sizeof batch, &count);
    if (st != MM_JOURNAL_OK) return st;
    for (int attempt = 0; attempt < 2; ++attempt) {
        st = mm_peer_receive(peer, 11, batch, count, receipt, sizeof receipt, &size);
        if (st != MM_JOURNAL_OK) return st;
        // First reply is lost. Retrying the batch retains the same record identities.
    }
    st = mm_peer_confirm(origin, 22, receipt, size);
    if (st == MM_JOURNAL_OK && mm_journal_confirmed(origin) != seq) return MM_JOURNAL_CONFLICT;
    if (st == MM_JOURNAL_OK) st = mm_journal_reclaim(origin);
    if (st == MM_JOURNAL_OK)
        Serial.printf("Peer holds through %llu after retry\n", (unsigned long long)seq);
    return st;
}
void setup() {
    Serial.begin(115200);
    if (!LittleFS.begin(false)) {
        Serial.println("Mount a formatted LittleFS partition to run PeerRetry.");
        return;
    }
    static const char key[] = "sensor\x1f" "demo\x1f";
    mm_journal_config config = {{11, 1, key, sizeof(key) - 1}, MM_JOURNAL_ORIGIN, MM_DESTINATION, 22, 22, 0};
    const mm_volume_options options = {16, 1024, MM_KEEP_OLD};
    mm_journal *origin = nullptr, *peer = nullptr;
    auto st = mm_journal_open_volume(&origin, "/littlefs/origin.mm", &config, &options, true);
    if (st == MM_JOURNAL_OK) {
        config.mode = MM_JOURNAL_RECEIVER;
        st = mm_journal_open_volume(&peer, "/littlefs/peer.mm", &config, &options, true);
        if (st == MM_JOURNAL_OK) {
            st = exercise(origin, peer);
            mm_journal_close(peer);
        }
        mm_journal_close(origin);
    }
    Serial.println(mm_journal_status_name(st));
}
void loop() { delay(1000); }
