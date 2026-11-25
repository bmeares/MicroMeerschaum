// SPDX-License-Identifier: Apache-2.0
/* Compact checkpoint encoding. Only used by journal.c. */
#ifndef MM_STORAGE_ROUTES_H
#define MM_STORAGE_ROUTES_H

#define ROUTE_FIXED 25u
#define ROUTE_HOP 17u
static size_t route_size(const uint8_t *p) { return ROUTE_FIXED + ROUTE_HOP * p[24]; }

static bool valid_routes(const uint8_t *p, size_t n)
{
    while (n) {
        if (n < ROUTE_FIXED || p[24] > MM_BATCH_MAX_HOPS || n < route_size(p) ||
            !mm_get64(p) || mm_get64(p + 8) < mm_get64(p)) return false;
        for (unsigned i = 0; i < p[24]; i++)
            if (p[ROUTE_FIXED + i * ROUTE_HOP + 16] > MM_TRANSPORT_USB) return false;
        size_t used = route_size(p);
        p += used;
        n -= used;
    }
    return true;
}

static void decode_route(const uint8_t *p, mm_route_range *r)
{
    r->first = mm_get64(p);
    r->last = mm_get64(p + 8);
    r->sender = mm_get64(p + 16);
    r->hop_count = p[24];
    p += ROUTE_FIXED;
    for (size_t i = 0; i < r->hop_count; ++i, p += ROUTE_HOP)
        r->hops[i] = (mm_hop){mm_get64(p), mm_get_i64(p + 8), (mm_transport)p[16]};
}

/* Merge touching intervals on the same authenticated path. Timestamp and
 * transport are part of the reported observation. Distinct paths survive. */
static mm_journal_status merge_route(const uint8_t *old, size_t old_len,
                                     const mm_route_range *r, uint8_t *out, size_t *len)
{
    uint8_t one[ROUTE_FIXED + MM_BATCH_MAX_HOPS * ROUTE_HOP];
    mm_put64(one + 16, r->sender);
    one[24] = (uint8_t)r->hop_count;
    uint8_t *hop = one + ROUTE_FIXED;
    for (size_t i = 0; i < r->hop_count; ++i, hop += ROUTE_HOP) {
        mm_put64(hop, r->hops[i].node);
        mm_put64(hop + 8, (uint64_t)r->hops[i].received_us);
        hop[16] = (uint8_t)r->hops[i].transport;
    }
    uint64_t lo = r->first, hi = r->last;
    size_t used = route_size(one), pos = 0;
    for (size_t i = 0; i < old_len; i += route_size(old + i)) {
        const uint8_t *p = old + i;
        size_t n = route_size(p);
        uint64_t a = mm_get64(p), b = mm_get64(p + 8);
        if (n == used && !memcmp(one + 16, p + 16, n - 16) &&
            !separated(b, lo) && !separated(hi, a)) {
            if (lo >= a && hi <= b) { *len = 0; return MM_JOURNAL_OK; }
            if (a < lo) lo = a;
            if (b > hi) hi = b;
        } else {
            memcpy(out + pos, p, n);
            pos += n;
        }
    }
    if (used > MM_ROUTE_BYTES - pos) return MM_JOURNAL_FULL;
    mm_put64(one, lo);
    mm_put64(one + 8, hi);
    memcpy(out + pos, one, used);
    *len = pos + used;
    return MM_JOURNAL_OK;
}
#endif
