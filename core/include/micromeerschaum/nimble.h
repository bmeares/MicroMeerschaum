// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_NIMBLE_H
#define MICROMEERSCHAUM_NIMBLE_H
#include "micromeerschaum/gatt.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Register once after NimBLE initialization and before host startup. Resolve an
 * authorized connection to its own server state on every access. Reset that
 * state on disconnect or any change of authenticated identity. */
typedef mm_gatt_server *(*mm_nimble_session)(void *ctx, uint16_t conn);
int mm_nimble_register(mm_nimble_session session, void *ctx);

typedef bool (*mm_nimble_authorized)(void *ctx, uint16_t conn);
typedef struct {
    uint16_t conn, write_handle, read_handle;
    uint32_t timeout_ms;
    mm_nimble_authorized authorized;
    void *ctx;
} mm_nimble_peer;
/* Call from a firmware task other than the NimBLE host. Resolve characteristics
 * once per connection; serialize operations and invalidate handles on disconnect.
 * On an I/O failure, disconnect before retrying the batch on a fresh session. */
mm_journal_status mm_nimble_discover(mm_nimble_peer *peer);
mm_journal_status mm_nimble_exchange(void *peer, const uint8_t *batch, size_t size,
                                    uint8_t *reply, size_t cap, size_t *used);
#ifdef __cplusplus
}
#endif
#endif
