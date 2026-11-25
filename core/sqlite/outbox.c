// SPDX-License-Identifier: Apache-2.0
#include "internal.h"

mm_journal_status mm_sqlite_read(mm_sqlite *db, uint64_t seq, uint8_t *out, size_t cap, size_t *used)
{
    if (!used) return MM_JOURNAL_INVALID;
    *used = 0;
    if (!db || !seq || seq > INT64_MAX || (!out && cap)) return MM_JOURNAL_INVALID;
    if (db->uncertain) return MM_JOURNAL_UNCERTAIN;
    sqlite3_stmt *s = NULL;
    int rc = mm_sql_prepare(db, "SELECT record FROM mm_out WHERE seq=?", &s);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s, 1, (int64_t)seq);
    if (rc == SQLITE_OK) rc = sqlite3_step(s);
    mm_journal_status st = mm_sql_status(rc);
    if (rc == SQLITE_DONE) st = MM_JOURNAL_NOT_FOUND;
    if (rc == SQLITE_ROW) {
        const void *record = sqlite3_column_blob(s, 0);
        size_t len = (size_t)sqlite3_column_bytes(s, 0), consumed;
        mm_record decoded;
        if (!record || mm_record_decode(record, len, &decoded, &consumed) != MM_OK ||
            decoded.seq != seq || consumed != len) st = MM_JOURNAL_CORRUPT;
        else {
            *used = len;
            if (cap < len) st = MM_JOURNAL_TOO_SMALL;
            else memcpy(out, record, len);
        }
    }
    sqlite3_finalize(s);
    return st;
}

mm_journal_status mm_sqlite_batch_after(mm_sqlite *db, uint64_t after, uint32_t max_records,
                                        uint8_t *out, size_t cap, size_t *used)
{
    if (!used) return MM_JOURNAL_INVALID;
    *used = 0;
    if (!db || !out) return MM_JOURNAL_INVALID;
    uint64_t high, confirmed;
    mm_journal_status st = mm_sqlite_positions(db, &high, &confirmed);
    if (st != MM_JOURNAL_OK) return st;
    if (after > high) return MM_JOURNAL_INVALID;
    if (after < confirmed && max_records) return MM_JOURNAL_NOT_FOUND;
    mm_v1_batch batch = {.stream=db->config.stream, .destination=db->config.destination};
    size_t header = 44 + batch.stream.key_len;
    size_t offset = header;
    if (cap > header + MM_JOURNAL_SEGMENT_MAX-MM_SEGMENT_HEADER_MAX)
        cap = header + MM_JOURNAL_SEGMENT_MAX-MM_SEGMENT_HEADER_MAX;
    while (after < high && batch.count < max_records) {
        size_t n;
        st = mm_sqlite_read(db, ++after, cap >= offset ? out + offset : NULL,
                            cap >= offset ? cap-offset : 0, &n);
        if (st == MM_JOURNAL_TOO_SMALL) {
            if (batch.count) break;
            *used = offset+n;
        }
        if (st != MM_JOURNAL_OK) return st == MM_JOURNAL_NOT_FOUND ? MM_JOURNAL_CORRUPT : st;
        offset += n;
        ++batch.count;
    }
    if (header > cap) { *used = header; return MM_JOURNAL_TOO_SMALL; }
    if (mm_v1_batch_header(out,cap,&batch) != header) return MM_JOURNAL_CORRUPT;
    *used = offset;
    return MM_JOURNAL_OK;
}

mm_journal_status mm_sqlite_batch(mm_sqlite *db, uint8_t *out, size_t cap, size_t *used)
{
    if (!used) return MM_JOURNAL_INVALID;
    *used = 0;
    uint64_t high, confirmed;
    mm_journal_status st = mm_sqlite_positions(db, &high, &confirmed);
    if (st != MM_JOURNAL_OK) return st;
    if (high == confirmed) return MM_JOURNAL_NOT_FOUND;
    return mm_sqlite_batch_after(db, confirmed, UINT32_MAX, out, cap, used);
}

mm_journal_status mm_sqlite_confirm(mm_sqlite *db, uint64_t issuer, const mm_receipt *r)
{
    if (!db || !r || !r->stream.key || mm_stream_check(&r->stream) != MM_OK ||
        !mm_stream_equal(&db->config.stream,&r->stream) || issuer != db->config.issuer ||
        issuer != r->issuer || r->role != db->config.role || r->destination != db->config.destination)
        return MM_JOURNAL_INVALID;
    mm_journal_status st = mm_sql_begin(db);
    if (st != MM_JOURNAL_OK) return st;
    uint64_t high = 0, confirmed = 0;
    st = mm_sqlite_positions(db, &high, &confirmed);
    if (st == MM_JOURNAL_OK && r->through > high) st = MM_JOURNAL_INVALID;
    if (st == MM_JOURNAL_OK && r->through > confirmed) {
        const char *queries[] = {"UPDATE mm_meta SET confirmed=? WHERE rowid=1",
                                 "DELETE FROM mm_out WHERE seq<=?"};
        for (size_t i = 0; i < 2 && st == MM_JOURNAL_OK; ++i) {
            sqlite3_stmt *s = NULL;
            int rc = mm_sql_prepare(db,queries[i],&s);
            if (rc == SQLITE_OK) rc = sqlite3_bind_int64(s,1,(int64_t)r->through);
            if (rc == SQLITE_OK) rc = sqlite3_step(s);
            sqlite3_finalize(s);
            st = mm_sql_status(rc);
        }
    }
    return mm_sql_finish(db, st);
}
