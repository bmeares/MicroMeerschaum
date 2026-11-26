// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_PIPE_HPP
#define MICROMEERSCHAUM_PIPE_HPP

#include <type_traits>
#include "micromeerschaum/pipe.h"
#if defined(MICROMEERSCHAUM_SQLITE_H) || defined(MM_WITH_SQLITE)
#include "micromeerschaum/sqlite.h"
#endif

namespace mm {
template<size_t N, typename T>
inline typename std::enable_if<std::is_same<T, bool>::value, mm_field>::type
field(const char (&name)[N], T b) {
    mm_field f = {{name, N - 1}, {}};
    f.value.type = MM_VALUE_BOOL;
    f.value.as.boolean = b;
    return f;
}

template<size_t N, typename T>
inline typename std::enable_if<std::is_integral<T>::value && std::is_signed<T>::value,
                               mm_field>::type
field(const char (&name)[N], T n) {
    static_assert(sizeof(T) <= sizeof(int64_t), "integer must fit in signed 64 bits");
    mm_field f = {{name, N - 1}, {}};
    f.value.type = MM_VALUE_INT;
    f.value.as.integer = static_cast<int64_t>(n);
    return f;
}

template<size_t N, typename T>
inline typename std::enable_if<std::is_floating_point<T>::value, mm_field>::type
field(const char (&name)[N], T x) {
    mm_field f = {{name, N - 1}, {}};
    static_assert(sizeof(T) <= sizeof(double), "real value must fit in binary64");
    f.value.type = MM_VALUE_FLOAT;
    f.value.as.real = x;
    return f;
}

template<size_t N>
inline mm_field field(const char (&name)[N], mm_text text) {
    mm_field f = {{name, N - 1}, {}};
    f.value.type = MM_VALUE_TEXT;
    f.value.as.text = text;
    return f;
}

template<size_t N, size_t M>
inline mm_field field(const char (&name)[N], const char (&text)[M]) {
    return field(name, mm_text{text, M - 1});
}

template<size_t N>
inline mm_field field(const char (&name)[N], decltype(nullptr)) {
    return mm_field{{name, N - 1}, {}};
}

template<size_t N>
inline mm_field time_field(const char (&name)[N], int64_t unix_us) {
    mm_field f = {{name, N - 1}, {}};
    f.value.type = MM_VALUE_TIME;
    f.value.as.timestamp_us = unix_us;
    return f;
}

template<size_t N>
inline mm_row row(const mm_field (&fields)[N]) { return mm_row{fields, N}; }

class Pipe {
    mm_pipe pipe_{};
public:
    mm_journal_status open(const char *connector, const char *metric, const char *location,
                           mm_pipe_target target, uint8_t *scratch, size_t capacity) {
        return mm_pipe_open(&pipe_, connector, metric, location, target, scratch, capacity);
    }
    mm_sync_result sync(mm_row row, uint64_t mono_us = 0, uint32_t boot = 0) {
        return mm_pipe_sync_rows(&pipe_, &row, 1, mono_us, boot);
    }
    template<size_t N>
    mm_sync_result sync(const mm_row (&rows)[N], uint64_t mono_us = 0, uint32_t boot = 0) {
        return mm_pipe_sync_rows(&pipe_, rows, N, mono_us, boot);
    }
    mm_sync_result sync_binary(const void *bytes, size_t len, uint64_t mono_us = 0,
                               uint32_t boot = 0) {
        return mm_pipe_sync_binary(&pipe_, bytes, len, mono_us, boot);
    }
    mm_journal_status bind_source(const char *name, mm_pipe_fetch_fn fetch, void *context) {
        return mm_pipe_bind_source(&pipe_, name, fetch, context);
    }
    mm_sync_result sync(uint64_t mono_us = 0, uint32_t boot = 0) {
        return mm_pipe_sync_source(&pipe_, mono_us, boot);
    }
};

#if defined(MICROMEERSCHAUM_SQLITE_H) || defined(MM_WITH_SQLITE)
inline mm_sqlite_query sqlite_query(size_t limit = MM_SQLITE_MAX_LIMIT, bool desc = false,
                                    const mm_value *begin = nullptr, const mm_value *end = nullptr,
                                    const mm_sqlite_filter *filters = nullptr,
                                    size_t filter_count = 0) {
    mm_sqlite_query q{};
    q.begin = begin;
    q.end = end;
    q.filters = filters;
    q.filter_count = filter_count;
    q.limit = limit;
    q.desc = desc;
    return q;
}

template<size_t N>
inline mm_sqlite_query sqlite_query(const mm_sqlite_filter (&filters)[N],
                                    size_t limit = MM_SQLITE_MAX_LIMIT, bool desc = false,
                                    const mm_value *begin = nullptr,
                                    const mm_value *end = nullptr) {
    return sqlite_query(limit, desc, begin, end, filters, N);
}

inline const mm_sqlite_definition *sqlite_definition(const mm_sqlite *db) {
    return mm_sqlite_definition_get(db);
}

inline mm_journal_status sqlite_rowcount(mm_sqlite *db, uint64_t &count,
                                         const mm_sqlite_query &query = mm_sqlite_query{}) {
    return mm_sqlite_get_rowcount(db, &query, &count);
}

inline mm_journal_status sqlite_sync_time(mm_sqlite *db, mm_value &result,
                                          const mm_sqlite_query &query = mm_sqlite_query{}) {
    return mm_sqlite_get_sync_time(db, &query, &result);
}

class SqliteCursor {
    mm_sqlite_cursor *cursor_ = nullptr;
public:
    SqliteCursor() = default;
    ~SqliteCursor() { close(); }
    SqliteCursor(const SqliteCursor &) = delete;
    SqliteCursor &operator=(const SqliteCursor &) = delete;
    SqliteCursor(SqliteCursor &&other) noexcept : cursor_(other.cursor_) {
        other.cursor_ = nullptr;
    }
    SqliteCursor &operator=(SqliteCursor &&other) noexcept {
        if (this != &other) {
            close();
            cursor_ = other.cursor_;
            other.cursor_ = nullptr;
        }
        return *this;
    }
    void close() {
        mm_sqlite_cursor_close(cursor_);
        cursor_ = nullptr;
    }
    mm_journal_status next(mm_row &row) { return mm_sqlite_next(cursor_, &row); }
    mm_sqlite_cursor *get() const { return cursor_; }
    explicit operator bool() const { return cursor_ != nullptr; }
    mm_sqlite_cursor **reset_and_put() {
        close();
        return &cursor_;
    }
};

class SqlitePipe : public Pipe {
    mm_sqlite *db_ = nullptr;
public:
    using Pipe::open;
    mm_journal_status open(const char *connector, const char *metric, const char *location,
                           mm_sqlite *db, uint8_t *scratch, size_t capacity) {
        db_ = nullptr;
        mm_journal_status st = Pipe::open(connector, metric, location, mm_sqlite_target(db),
                                          scratch, capacity);
        if (st == MM_JOURNAL_OK) db_ = db;
        return st;
    }
    mm_sqlite *sqlite() const { return db_; }
    const mm_sqlite_definition *definition() const { return mm_sqlite_definition_get(db_); }
    mm_journal_status get_rowcount(uint64_t &count,
                                   const mm_sqlite_query &query = mm_sqlite_query{}) const {
        return mm_sqlite_get_rowcount(db_, &query, &count);
    }
    mm_journal_status get_sync_time(mm_value &result,
                                    const mm_sqlite_query &query = mm_sqlite_query{}) const {
        return mm_sqlite_get_sync_time(db_, &query, &result);
    }
    mm_journal_status get_docs(SqliteCursor &cursor, const mm_sqlite_query &query) const {
        return mm_sqlite_get_docs(db_, &query, cursor.reset_and_put());
    }
    mm_journal_status get_docs(SqliteCursor &cursor, size_t limit = MM_SQLITE_MAX_LIMIT,
                               bool desc = false) const {
        return get_docs(cursor, sqlite_query(limit, desc));
    }
};
#endif
} // namespace mm
#endif
