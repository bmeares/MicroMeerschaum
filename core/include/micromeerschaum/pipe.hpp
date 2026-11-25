// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_PIPE_HPP
#define MICROMEERSCHAUM_PIPE_HPP

#include <type_traits>
#include "micromeerschaum/pipe.h"

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
} // namespace mm
#endif
