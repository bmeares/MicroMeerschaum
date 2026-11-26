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

#ifdef MM_WITH_SQLITE
static void sqlite_wrapper()
{
    char dir[] = "/tmp/mm-pipe-sql-XXXXXX";
    CHECK(mkdtemp(dir));
    std::string db_path = std::string(dir) + "/device.db";
    char key[MM_KEY_MAX];
    size_t key_len;
    CHECK(mm_pipe_key(key, sizeof key, &key_len, "sensor", "temperature", "lab") == MM_OK);
    const mm_journal_config cfg = {{3, 1, key, key_len}, MM_JOURNAL_ORIGIN,
                                   MM_DESTINATION, 9, 9, 0};
    const mm_sqlite_column cols[] = {
        {"tick", MM_VALUE_INT, false},
        {"station", MM_VALUE_TEXT, true},
        {"celsius", MM_VALUE_FLOAT, false},
    };
    const mm_sqlite_definition def = {cols, 3, 0, false};
    mm_sqlite *db = nullptr;
    CHECK(mm_sqlite_open(&db, db_path.c_str(), &cfg, &def) == MM_JOURNAL_OK);

    mm::SqlitePipe pipe;
    uint8_t scratch[256];
    CHECK(pipe.open("sensor", "wrong", "lab", db, scratch, sizeof scratch) == MM_JOURNAL_CONFLICT);
    CHECK(pipe.sqlite() == nullptr);
    CHECK(pipe.open("sensor", "temperature", "lab", db, scratch, sizeof scratch) == MM_JOURNAL_OK);
    CHECK(pipe.sqlite() == db);

    const mm_sqlite_definition *d = pipe.definition();
    CHECK(d != nullptr && d == mm::sqlite_definition(db));
    CHECK(d->count == 3 && d->axis == 0 && !d->autotime);
    CHECK(std::strcmp(d->columns[1].name, "station") == 0 && d->columns[1].key);

    const mm_field r1[] = {mm::field("tick", 1), mm::field("station", "A"), mm::field("celsius", 20.0)};
    const mm_field r2[] = {mm::field("tick", 2), mm::field("station", "A"), mm::field("celsius", 21.0)};
    const mm_field r3[] = {mm::field("tick", 2), mm::field("station", "B"), mm::field("celsius", 22.0)};
    const mm_field r4[] = {mm::field("tick", 1), mm::field("station", "A"), mm::field("celsius", 25.0)};
    const mm_row rows[] = {mm::row(r1), mm::row(r2), mm::row(r3), mm::row(r4)};
    CHECK(pipe.sync(rows, 10, 1).confirmed == 4);

    uint64_t count = 0;
    CHECK(pipe.get_rowcount(count) == MM_JOURNAL_OK && count == 3);
    CHECK(mm::sqlite_rowcount(db, count) == MM_JOURNAL_OK && count == 3);

    mm_value st_a{};
    st_a.type = MM_VALUE_TEXT;
    st_a.as.text = mm_text{"A", 1};
    const mm_sqlite_filter flt[] = {{"station", &st_a, 1}};
    CHECK(pipe.get_rowcount(count, mm::sqlite_query(flt)) == MM_JOURNAL_OK && count == 2);

    const mm_sqlite_filter empty_flt[] = {{"station", nullptr, 0}};
    CHECK(pipe.get_rowcount(count, mm::sqlite_query(empty_flt)) == MM_JOURNAL_OK && count == 0);

    const mm_sqlite_filter bad_flt[] = {{"unknown", &st_a, 1}};
    CHECK(pipe.get_rowcount(count, mm::sqlite_query(bad_flt)) == MM_JOURNAL_INVALID);

    mm_value latest{};
    CHECK(pipe.get_sync_time(latest, mm::sqlite_query(flt)) == MM_JOURNAL_OK);
    CHECK(latest.type == MM_VALUE_INT && latest.as.integer == 2);
    CHECK(mm::sqlite_sync_time(db, latest) == MM_JOURNAL_OK && latest.as.integer == 2);

    {
        mm::SqliteCursor cursor;
        mm_row row{};
        CHECK(pipe.get_docs(cursor, 2, true) == MM_JOURNAL_OK && static_cast<bool>(cursor));
        CHECK(cursor.next(row) == MM_JOURNAL_OK && row.fields[0].value.as.integer == 2 &&
              row.fields[2].value.as.real == 22.0);
        CHECK(cursor.next(row) == MM_JOURNAL_OK && row.fields[0].value.as.integer == 2 &&
              row.fields[2].value.as.real == 21.0);
        CHECK(cursor.next(row) == MM_JOURNAL_NOT_FOUND);

        mm_value begin{};
        begin.type = MM_VALUE_INT;
        begin.as.integer = 1;
        mm_value end{};
        end.type = MM_VALUE_INT;
        end.as.integer = 3;
        CHECK(pipe.get_docs(cursor, mm::sqlite_query(flt, 10, true, &begin, &end)) == MM_JOURNAL_OK);
        CHECK(cursor.next(row) == MM_JOURNAL_OK && row.fields[0].value.as.integer == 2 &&
              row.fields[2].value.as.real == 21.0);
        CHECK(cursor.next(row) == MM_JOURNAL_OK && row.fields[0].value.as.integer == 1 &&
              row.fields[2].value.as.real == 25.0);
        CHECK(cursor.next(row) == MM_JOURNAL_NOT_FOUND);
    }

    // Cursor closed via RAII, so further writes succeed immediately.
    const mm_field r5[] = {mm::field("tick", 3), mm::field("station", "C"), mm::field("celsius", 23.0)};
    CHECK(pipe.sync(mm::row(r5), 20, 1).confirmed == 1);
    CHECK(pipe.get_rowcount(count) == MM_JOURNAL_OK && count == 4);

    mm_sqlite_close(db);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    CHECK(!ec);
}
#endif

int main()
{
    source_first_use();
    partial_commit();
    binary_driver();
#ifdef MM_WITH_SQLITE
    sqlite_wrapper();
#endif
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
