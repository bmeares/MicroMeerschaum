// SPDX-License-Identifier: Apache-2.0
/* Add relay.c to an ESP-IDF NimBLE application's component sources. */
#ifndef MM_EXAMPLE_RELAY_H
#define MM_EXAMPLE_RELAY_H
#include "host/ble_gap.h"
#include "micromeerschaum/nimble.h"
#include "micromeerschaum/sender.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    ble_addr_t identity_address; /* Enrolled identity address, native NimBLE byte order. */
    uint64_t node;
} peer_enrollment;

typedef struct {
    mm_journal *journal;
    peer_enrollment upstream;
    mm_gatt_server incoming;
    uint16_t inbound_handle;
    SemaphoreHandle_t journal_lock;
    uint8_t receive_buffer[4096], send_buffer[4096], reply[MM_RECEIPT_MAX];
} peer_role;

/* Keep this context alive for the service lifetime. Install after nimble_port_init()
 * and before host startup for a receiver or relay. An origin needs only init(). */
mm_journal_status peer_role_init(peer_role *role, mm_journal *journal);
int peer_role_register(peer_role *role, peer_enrollment upstream);
void peer_role_disconnect(peer_role *role, uint16_t handle);
/* Called by an application task after a bonded central connection is ready. */
mm_journal_status peer_role_deliver(peer_role *role, uint16_t handle,
                                    peer_enrollment peer, mm_send_result *progress);
#endif
