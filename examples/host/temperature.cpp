// SPDX-License-Identifier: Apache-2.0
#include <cinttypes>
#include <cstdio>
#include "micromeerschaum/pipe.hpp"
#include "micromeerschaum/journal_pipe.h"

// Run with a provisioned private directory. The radio may stay off.
int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s JOURNAL_DIRECTORY\n", argv[0]);
        return 2;
    }
    char key[MM_KEY_MAX];
    size_t key_len;
    if (mm_pipe_key(key, sizeof key, &key_len, "sensor", "temperature", nullptr) != MM_OK)
        return 1;
    const mm_journal_config config = {{1, 1, key, key_len}, MM_JOURNAL_ORIGIN,
                                      MM_DESTINATION, 2, 2, 0};
    mm_journal *journal = nullptr;
    auto status = mm_journal_open(&journal, argv[1], &config, true);
    if (status == MM_JOURNAL_OK) {
        uint8_t scratch[128];
        mm::Pipe pipe;
        status = pipe.open("sensor", "temperature", nullptr, mm_pipe_journal_target(journal),
                           scratch, sizeof scratch);
        if (status == MM_JOURNAL_OK) {
            const mm_field first[] = {mm::field("tick", 41), mm::field("celsius", 21.4)};
            const mm_field second[] = {mm::field("tick", 42), mm::field("celsius", 21.7)};
            const mm_row rows[] = {mm::row(first), mm::row(second)};
            const auto result = pipe.sync(rows);
            status = result.status;
            std::printf("Committed %zu rows locally\n", result.confirmed);
            if (result.uncertain_count)
                std::fprintf(stderr, "Reconcile %zu inputs starting at sequence %" PRIu64 "\n",
                             result.uncertain_count, result.uncertain_first);
        }
        mm_journal_close(journal);
    }
    if (status != MM_JOURNAL_OK) std::fprintf(stderr, "%s\n", mm_journal_status_name(status));
    return status == MM_JOURNAL_OK ? 0 : 1;
}
