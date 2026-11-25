// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_PEER_H
#define MICROMEERSCHAUM_PEER_H

#include "micromeerschaum/journal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Scoped exchange on a journal. The caller transports the
 * bytes and supplies the identity established by the authenticated link.
 * The link authorizes the immediate sender for this stream and final destination.
 * A receiver with role MM_CUSTODY retains records for onward delivery.
 * Serialize operations on a journal and use separate input and output buffers. */

/* Fill one batch with whole records after the given sequence, up to max_records
 * and cap. Normally after = mm_journal_confirmed(origin). A zero record limit
 * asks for the peer's current receipt. If the next record won't fit, return
 * TOO_SMALL. On any failure *used is zero and the buffer must be discarded. */
mm_journal_status mm_peer_batch(mm_journal *origin, uint64_t after, uint32_t max_records,
                                uint8_t *out, size_t cap, size_t *used);

/* Validate and commit the batch before issuing its cumulative receipt. On any
 * failure *reply_len is zero. Retrying identical bytes is safe after reopen. */
mm_journal_status mm_peer_receive(mm_journal *receiver, uint64_t authenticated_sender,
                                  const uint8_t *batch, size_t batch_len,
                                  uint8_t *reply, size_t reply_cap, size_t *reply_len);

/* Persist the confirmed position. Reclamation is a separate journal operation. */
mm_journal_status mm_peer_confirm(mm_journal *origin, uint64_t authenticated_issuer,
                                  const uint8_t *reply, size_t len);

#ifdef __cplusplus
}
#endif
#endif
