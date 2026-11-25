// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_SENDER_H
#define MICROMEERSCHAUM_SENDER_H

#include "micromeerschaum/peer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The exchange returns the complete binary receipt on an authenticated link.
 * An interrupted response is an error. The next window retries the same bytes. */
typedef mm_journal_status (*mm_exchange_fn)(void *ctx, const uint8_t *data, size_t len,
                                             uint8_t *reply, size_t cap, size_t *reply_len);
typedef struct {
    size_t batches;
    uint64_t through;
} mm_send_result;

/* Drain up to max_batches from the current confirmed position using caller
 * storage. Returns confirmed progress even when a later batch fails. Storage
 * reclamation and the radio wake interval belong to the caller. */
mm_journal_status mm_send_window(mm_journal *origin, uint64_t authenticated_issuer,
                                 mm_exchange_fn exchange, void *ctx,
                                 uint8_t *buf, size_t capacity, size_t max_batches,
                                 mm_send_result *result);

#ifdef __cplusplus
}
#endif
#endif
