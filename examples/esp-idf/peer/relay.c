// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include "relay.h"

static bool authorized(void *ctx, uint16_t handle)
{
    const peer_enrollment *peer = ctx;
    struct ble_gap_conn_desc desc;
    return ble_gap_conn_find(handle, &desc) == 0 && desc.sec_state.encrypted &&
           desc.sec_state.authenticated && desc.sec_state.bonded &&
           ble_addr_cmp(&desc.peer_id_addr, &peer->identity_address) == 0;
}

static mm_journal_status receive(void *ctx, const uint8_t *data, size_t len, uint8_t *reply,
                                 size_t cap, size_t *used)
{
    peer_role *role = ctx;
    /* The host task must remain able to complete an outgoing central operation. */
    if (!xSemaphoreTake(role->journal_lock, 0)) return MM_JOURNAL_BUSY;
    mm_journal_status st = mm_peer_receive(role->journal, role->upstream.node, data, len,
                                            reply, cap, used);
    xSemaphoreGive(role->journal_lock);
    return st;
}

static mm_gatt_server *session(void *ctx, uint16_t handle)
{
    peer_role *role = ctx;
    if (!authorized(&role->upstream, handle)) {
        peer_role_disconnect(role, handle);
        return NULL;
    }
    if (role->inbound_handle == BLE_HS_CONN_HANDLE_NONE) role->inbound_handle = handle;
    return role->inbound_handle == handle ? &role->incoming : NULL;
}

mm_journal_status peer_role_init(peer_role *role, mm_journal *journal)
{
    memset(role, 0, sizeof *role);
    role->journal = journal;
    role->inbound_handle = BLE_HS_CONN_HANDLE_NONE;
    role->journal_lock = xSemaphoreCreateMutex();
    if (!role->journal_lock) return MM_JOURNAL_NOMEM;
    return mm_gatt_init(&role->incoming, role->receive_buffer, sizeof role->receive_buffer,
                        role->reply, sizeof role->reply, receive, role);
}

int peer_role_register(peer_role *role, peer_enrollment upstream)
{
    role->upstream = upstream;
    return mm_nimble_register(session, role);
}

void peer_role_disconnect(peer_role *role, uint16_t handle)
{
    if (role->inbound_handle == handle) {
        role->inbound_handle = BLE_HS_CONN_HANDLE_NONE;
        mm_gatt_reset(&role->incoming);
    }
}

mm_journal_status peer_role_deliver(peer_role *role, uint16_t handle,
                                    peer_enrollment peer, mm_send_result *progress)
{
    mm_nimble_peer link = {.conn = handle, .timeout_ms = 10000,
                           .authorized = authorized, .ctx = &peer};
    *progress = (mm_send_result){0};
    mm_journal_status st = mm_nimble_discover(&link);
    if (st != MM_JOURNAL_OK) return st;
    if (!xSemaphoreTake(role->journal_lock, 0)) return MM_JOURNAL_BUSY;
    st = mm_send_window(role->journal, peer.node, mm_nimble_exchange, &link,
                        role->send_buffer, sizeof role->send_buffer, 4, progress);
    if (st == MM_JOURNAL_OK) st = mm_journal_reclaim(role->journal);
    xSemaphoreGive(role->journal_lock);
    return st;
}
