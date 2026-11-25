// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/sender.h"

mm_journal_status mm_send_window(mm_journal *origin, uint64_t authenticated_issuer,
                                 mm_exchange_fn exchange, void *ctx,
                                 uint8_t *buf, size_t capacity, size_t max_batches,
                                 mm_send_result *result)
{
    if (!result) return MM_JOURNAL_INVALID;
    *result = (mm_send_result){0};
    if (!origin || !exchange || !buf || !capacity) return MM_JOURNAL_INVALID;
    const mm_journal_config *config = mm_journal_config_get(origin);
    bool relay = config->mode == MM_JOURNAL_RECEIVER && config->role == MM_CUSTODY;
    if ((config->mode != MM_JOURNAL_ORIGIN && !relay) ||
        (relay ? config->destination : config->issuer) != authenticated_issuer)
        return MM_JOURNAL_INVALID;
    result->through = mm_journal_confirmed(origin);
    if (mm_journal_needs_reopen(origin)) return MM_JOURNAL_UNCERTAIN;
    const uint64_t high = mm_journal_high(origin);
    uint8_t reply[MM_RECEIPT_MAX];
    while (result->batches < max_batches && result->through < high) {
        size_t used = 0, reply_len = 0;
        mm_journal_status status = mm_peer_batch(origin, result->through, UINT32_MAX,
                                                  buf, capacity, &used);
        if (status != MM_JOURNAL_OK) return status;
        status = exchange(ctx, buf, used, reply, sizeof reply, &reply_len);
        if (status != MM_JOURNAL_OK) return status;
        if (reply_len > sizeof reply) return MM_JOURNAL_INVALID;
        status = mm_peer_confirm(origin, authenticated_issuer, reply, reply_len);
        if (status != MM_JOURNAL_OK) return status;
        uint64_t confirmed = mm_journal_confirmed(origin);
        if (confirmed <= result->through) return MM_JOURNAL_CONFLICT;
        result->through = confirmed;
        ++result->batches;
    }
    return MM_JOURNAL_OK;
}
