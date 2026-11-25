// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_SESSION_GATT_H
#define MICROMEERSCHAUM_SESSION_GATT_H
#include "micromeerschaum/device_session.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*mm_session_peer_fn)(uint16_t handle, uint8_t address_type,
                                   const uint8_t identity_address[6], void *);

/* Register once after NimBLE initialization and before host startup. A peer
 * must subscribe, satisfy link security, and pass the application enrollment
 * check on its resolved identity. One subscribed connection at a time.
 * The session is dedicated to this worker for the lifetime of the host. */
int mm_session_gatt_register(mm_device_session *, mm_session_peer_fn, void *user);

struct ble_gap_event;
/* Forward the application's GAP events, including subscribe and disconnect. */
void mm_session_gatt_event(const struct ble_gap_event *);

#ifdef __cplusplus
}
#endif
#endif
