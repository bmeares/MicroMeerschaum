// SPDX-License-Identifier: Apache-2.0
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include "micromeerschaum/journal_pipe.h"
#include "micromeerschaum/pipe.hpp"

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

static mm_journal_status fetch(void *ctx, const mm_pipe *, size_t index, mm_row *row, bool *done)
{
    *done = index == 1;
    if (!*done) *row = *static_cast<const mm_row *>(ctx);
    return MM_JOURNAL_OK;
}

static void source_first_use()
{
    char dir[] = "/tmp/mm-source-XXXXXX";
    CHECK(mkdtemp(dir));
    char key[MM_KEY_MAX];
    size_t key_len;
    CHECK(mm_pipe_key(key, sizeof key, &key_len, "plugin:weather", "daily", "north") == MM_OK);
    const mm_journal_config cfg = {{7, 9, key, key_len}, MM_JOURNAL_ORIGIN,
                                   MM_DESTINATION, 2, 2, 0};
    mm_journal *j = nullptr;
    CHECK(mm_journal_open(&j, dir, &cfg, true) == MM_JOURNAL_OK);
    uint8_t scratch[64];
    mm::Pipe pipe;
    CHECK(pipe.open("plugin:weather", "daily", "north", mm_pipe_journal_target(j),
                    scratch, sizeof scratch) == MM_JOURNAL_OK);
    CHECK(pipe.sync().status == MM_JOURNAL_INVALID);
    const mm_field weather[] = {mm::time_field("at", -9), mm::field("sun", true)};
    mm_row row = mm::row(weather);
    CHECK(pipe.bind_source("other", fetch, &row) == MM_JOURNAL_INVALID);
    CHECK(pipe.bind_source("weather", fetch, &row) == MM_JOURNAL_OK);
    auto result = pipe.sync();
    CHECK(result.status == MM_JOURNAL_OK && result.confirmed == 1);
    // Explicitly supplying a row bypasses the registered source.
    result = pipe.sync(mm_row{nullptr, 0});
    CHECK(result.status == MM_JOURNAL_OK && result.confirmed == 1);
    mm_journal_close(j);
    CHECK(mm_journal_open(&j, dir, &cfg, false) == MM_JOURNAL_OK);
    uint8_t buf[100];
    size_t n, used, count;
    mm_record rec{};
    mm_field fields[2];
    CHECK(mm_journal_read(j, 1, buf, sizeof buf, &n) == MM_JOURNAL_OK);
    CHECK(mm_record_decode(buf, n, &rec, &used) == MM_OK);
    CHECK(mm_document_decode(rec.payload, rec.len, fields, 2, &count) == MM_OK && count == 2);
    CHECK(fields[0].value.type == MM_VALUE_TIME && fields[0].value.as.timestamp_us == -9);
    CHECK(mm_journal_read(j, 2, buf, sizeof buf, &n) == MM_JOURNAL_OK);
    CHECK(mm_record_decode(buf, n, &rec, &used) == MM_OK);
    CHECK(mm_document_decode(rec.payload, rec.len, nullptr, 0, &count) == MM_OK && count == 0);
    mm_journal_close(j);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    CHECK(!ec);
}

static int fail_second_directory_sync(int fd, void *ctx)
{
    struct stat info{};
    CHECK(fstat(fd, &info) == 0);
    if (S_ISDIR(info.st_mode) && ++*static_cast<unsigned *>(ctx) == 2) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

static void partial_commit()
{
    char dir[] = "/tmp/mm-partial-XXXXXX";
    CHECK(mkdtemp(dir));
    const char key[] = "sensor\x1f" "counts\x1f";
    const mm_journal_config cfg = {{1, 4, key, sizeof key - 1}, MM_JOURNAL_ORIGIN,
                                   MM_DESTINATION, 2, 2, 0};
    mm_journal *j = nullptr;
    CHECK(mm_journal_open(&j, dir, &cfg, true) == MM_JOURNAL_OK);
    mm_pipe pipe{};
    uint8_t scratch[12];  // Fits two empty documents per group.
    CHECK(mm_pipe_open(&pipe, "sensor", "counts", nullptr, mm_pipe_journal_target(j),
                       scratch, sizeof scratch) == MM_JOURNAL_OK);
    unsigned barriers = 0;
    mm_journal_set_barrier(j, fail_second_directory_sync, &barriers);
    const mm_row empty[4] = {};
    auto result = mm_pipe_sync_rows(&pipe, empty, 4, 717, 2);
    CHECK(result.status == MM_JOURNAL_UNCERTAIN && result.confirmed == 2);
    CHECK(result.uncertain_first == 3 && result.uncertain_count == 2);
    auto blocked = mm_pipe_sync_rows(&pipe, empty, 1, 717, 2);
    CHECK(blocked.status == MM_JOURNAL_UNCERTAIN && blocked.confirmed == 0);
    CHECK(blocked.uncertain_first == 0 && blocked.uncertain_count == 0);
    mm_journal_close(j);
    CHECK(mm_journal_open(&j, dir, &cfg, false) == MM_JOURNAL_OK);
    for (size_t i = 0; i < result.uncertain_count; ++i) {
        uint8_t buf[48];
        size_t n, used;
        mm_record rec{};
        CHECK(mm_journal_read(j, result.uncertain_first + i, buf, sizeof buf, &n) == MM_JOURNAL_OK);
        CHECK(mm_record_decode(buf, n, &rec, &used) == MM_OK);
        CHECK(rec.mono_us == 717 && rec.boot == 2 && rec.len == 6);
        CHECK(!std::memcmp(rec.payload, "MMDC\1\0", 6));
    }
    CHECK(mm_journal_high(j) == 4);
    mm_journal_close(j);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    CHECK(!ec);
}

static void binary_driver()
{
    char dir[] = "/tmp/mm-binary-XXXXXX";
    CHECK(mkdtemp(dir));
    const char key[] = "driver\x1f" "frames\x1f";
    const mm_journal_config cfg = {{1, 4, key, sizeof key - 1}, MM_JOURNAL_ORIGIN,
                                   MM_DESTINATION, 2, 2, 0};
    mm_journal *j = nullptr;
    CHECK(mm_journal_open(&j, dir, &cfg, true) == MM_JOURNAL_OK);
    mm::Pipe pipe;
    CHECK(pipe.open("driver", "frames", nullptr, mm_pipe_journal_target(j),
                    nullptr, 0) == MM_JOURNAL_OK);
    uint8_t data[] = {'M', 'M', 'D', 'C', 0xff};
    auto result = pipe.sync_binary(data, sizeof data, 345, 2);
    CHECK(result.status == MM_JOURNAL_OK && result.confirmed == 1);
    std::memset(data, 0, sizeof data);
    mm_journal_close(j);
    CHECK(mm_journal_open(&j, dir, &cfg, false) == MM_JOURNAL_OK);
    uint8_t buf[48];
    size_t n, used;
    mm_record rec{};
    CHECK(mm_journal_read(j, 1, buf, sizeof buf, &n) == MM_JOURNAL_OK);
    CHECK(mm_record_decode(buf, n, &rec, &used) == MM_OK);
    CHECK(rec.mono_us == 345 && rec.boot == 2 && rec.len == 5);
    CHECK(!std::memcmp(rec.payload, "MMDC\xff", 5));
    mm_journal_close(j);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    CHECK(!ec);
}

int main()
{
    source_first_use();
    partial_commit();
    binary_driver();
    char dir[] = "/tmp/mm-named-pipe-XXXXXX";
    CHECK(mkdtemp(dir));
    char key[MM_KEY_MAX];
    size_t key_len;
    CHECK(mm_pipe_key(key, sizeof key, &key_len, "sensor", "temperature", nullptr) == MM_OK);
    const mm_journal_config config = {{1, 5, key, key_len}, MM_JOURNAL_ORIGIN,
                                      MM_DESTINATION, 2, 2, 0};
    mm_journal *journal = nullptr;
    CHECK(mm_journal_open(&journal, dir, &config, true) == MM_JOURNAL_OK);
    mm::Pipe pipe;
    uint8_t scratch[128];
    CHECK(pipe.open("sensor", "elsewhere", nullptr, mm_pipe_journal_target(journal),
                    scratch, sizeof scratch) == MM_JOURNAL_CONFLICT);
    CHECK(pipe.open("sensor", "temperature", nullptr, mm_pipe_journal_target(journal),
                    scratch, sizeof scratch) == MM_JOURNAL_OK);
    const mm_field first[] = {mm::field("tick", 41), mm::field("celsius", 21.4)};
    const mm_field second[] = {mm::field("tick", 42), mm::field("celsius", 21.7)};
    const mm_row rows[] = {mm::row(first), mm::row(second)};
    auto result = pipe.sync(rows, 123, 5);
    CHECK(result.status == MM_JOURNAL_OK && result.confirmed == 2);
    mm_journal_close(journal);

    CHECK(mm_journal_open(&journal, dir, &config, false) == MM_JOURNAL_OK);
    CHECK(mm_journal_high(journal) == 2);
    for (uint64_t seq = 1; seq <= 2; ++seq) {
        uint8_t buf[128];
        size_t used, consumed, count;
        mm_record record{};
        mm_field fields[2];
        CHECK(mm_journal_read(journal, seq, buf, sizeof buf, &used) == MM_JOURNAL_OK);
        CHECK(mm_record_decode(buf, used, &record, &consumed) == MM_OK && consumed == used);
        CHECK(record.seq == seq && record.mono_us == 123 && record.boot == 5);
        CHECK(mm_document_decode(record.payload, record.len, fields, 2, &count) == MM_OK && count == 2);
        CHECK(fields[0].value.as.integer == static_cast<int64_t>(40 + seq));
        CHECK(fields[1].value.as.real == (seq == 1 ? 21.4 : 21.7));
    }
    mm_journal_close(journal);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    CHECK(!ec);
}
