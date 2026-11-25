// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/nimble.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"

#define UUID(n) BLE_UUID128_INIT(0x22,0x57,0xb5,0x19,0x51,0x82,0x3a,0x9d,0xe5,0x49,0xf1,0x5a,n,0,0xda,0xd9)
#define VALUE_CAP 244u
static const ble_uuid128_t service_id = UUID(1), write_id = UUID(2), read_id = UUID(3);
static mm_nimble_session resolve;
static void *resolve_ctx;

static int access_value(uint16_t conn, uint16_t handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)handle;
    (void)arg;
    mm_gatt_server *server = resolve ? resolve(resolve_ctx, conn) : NULL;
    if (!server) return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    uint8_t value[VALUE_CAP];
    size_t size = 0;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t n = OS_MBUF_PKTLEN(ctxt->om);
        if (n > sizeof value || os_mbuf_copydata(ctxt->om, 0, n, value))
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        return mm_gatt_write(server, value, n) == MM_JOURNAL_OK ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR ||
        mm_gatt_read(server, value, sizeof value, &size) != MM_JOURNAL_OK)
        return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, value, size) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static const struct ble_gatt_chr_def chars[] = {
    {.uuid = &write_id.u, .access_cb = access_value, .flags = BLE_GATT_CHR_F_WRITE},
    {.uuid = &read_id.u, .access_cb = access_value, .flags = BLE_GATT_CHR_F_READ},
    {0}
};
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &service_id.u, .characteristics = chars}, {0}
};

int mm_nimble_register(mm_nimble_session session, void *ctx)
{
    if (!session || resolve) return BLE_HS_EINVAL;
    int err = ble_gatts_count_cfg(services);
    if (!err) err = ble_gatts_add_svcs(services);
    if (!err) { resolve = session; resolve_ctx = ctx; }
    return err;
}

/* The stack may finish after the waiting task times out. Its reference keeps
 * the semaphore and bytes alive. No callback retains application buffers. */
typedef struct {
    SemaphoreHandle_t done;
    atomic_uint references;
    int result;
    uint16_t first, last, tx, rx, size;
    uint8_t value[VALUE_CAP];
} request;

static request *start(void)
{
    request *r = calloc(1, sizeof *r);
    if (r && !(r->done = xSemaphoreCreateBinary())) { free(r); return NULL; }
    if (r) atomic_init(&r->references, 2);
    return r;
}

static void release(request *r)
{
    if (atomic_fetch_sub(&r->references, 1) == 1) { vSemaphoreDelete(r->done); free(r); }
}

static int finish(request *r, int result)
{
    r->result = result;
    xSemaphoreGive(r->done);
    release(r);
    return 0;
}

static mm_journal_status wait_for(mm_nimble_peer *peer, request *r, int err)
{
    if (err) {
        release(r); /* No callback is scheduled on start failure. */
        return MM_JOURNAL_IO;
    }
    if (xSemaphoreTake(r->done, pdMS_TO_TICKS(peer->timeout_ms)) != pdTRUE || r->result ||
        !peer->authorized(peer->ctx, peer->conn)) return MM_JOURNAL_IO;
    return MM_JOURNAL_OK;
}

static int service_cb(uint16_t conn, const struct ble_gatt_error *e,
                      const struct ble_gatt_svc *svc, void *ctx)
{
    (void)conn;
    request *r = ctx;
    if (e->status) return finish(r, e->status == BLE_HS_EDONE && r->first ? 0 : e->status);
    if (svc) { r->first = svc->start_handle; r->last = svc->end_handle; }
    return 0;
}

static int char_cb(uint16_t conn, const struct ble_gatt_error *e,
                   const struct ble_gatt_chr *chr, void *ctx)
{
    (void)conn;
    request *r = ctx;
    if (e->status) return finish(r, e->status == BLE_HS_EDONE && r->tx && r->rx ? 0 : e->status);
    if (chr && !ble_uuid_cmp(&chr->uuid.u, &write_id.u)) r->tx = chr->val_handle;
    if (chr && !ble_uuid_cmp(&chr->uuid.u, &read_id.u)) r->rx = chr->val_handle;
    return 0;
}

static bool allowed(mm_nimble_peer *peer)
{
    return peer && peer->authorized && peer->timeout_ms && peer->authorized(peer->ctx, peer->conn);
}

mm_journal_status mm_nimble_discover(mm_nimble_peer *peer)
{
    if (!allowed(peer)) return MM_JOURNAL_INVALID;
    peer->write_handle = peer->read_handle = 0;
    request *r = start();
    if (!r) return MM_JOURNAL_NOMEM;
    int err = ble_gattc_disc_svc_by_uuid(peer->conn, &service_id.u, service_cb, r);
    mm_journal_status status = wait_for(peer, r, err);
    uint16_t first = 0, last = 0;
    if (status == MM_JOURNAL_OK) { first = r->first; last = r->last; }
    release(r);
    if (status != MM_JOURNAL_OK) return status;
    r = start();
    if (!r) return MM_JOURNAL_NOMEM;
    err = ble_gattc_disc_all_chrs(peer->conn, first, last, char_cb, r);
    status = wait_for(peer, r, err);
    if (status == MM_JOURNAL_OK) { peer->write_handle = r->tx; peer->read_handle = r->rx; }
    release(r);
    return status;
}

static int attr_cb(uint16_t conn, const struct ble_gatt_error *e, struct ble_gatt_attr *attr, void *ctx)
{
    (void)conn;
    request *r = ctx;
    int err = e->status;
    if (!err && attr && attr->om) {
        r->size = OS_MBUF_PKTLEN(attr->om);
        if (r->size > sizeof r->value || os_mbuf_copydata(attr->om, 0, r->size, r->value))
            err = BLE_HS_EMSGSIZE;
    }
    return finish(r, err);
}

static mm_journal_status write_value(void *ctx, const uint8_t *buf, size_t n)
{
    mm_nimble_peer *p = ctx;
    if (!allowed(p)) return MM_JOURNAL_INVALID;
    request *r = start();
    if (!r) return MM_JOURNAL_NOMEM;
    int err = ble_gattc_write_flat(p->conn, p->write_handle, buf, (uint16_t)n, attr_cb, r);
    mm_journal_status status = wait_for(p, r, err);
    release(r);
    return status;
}

static mm_journal_status read_value(void *ctx, uint8_t *buf, size_t cap, size_t *used)
{
    mm_nimble_peer *p = ctx;
    *used = 0;
    if (!allowed(p)) return MM_JOURNAL_INVALID;
    request *r = start();
    if (!r) return MM_JOURNAL_NOMEM;
    int err = ble_gattc_read(p->conn, p->read_handle, attr_cb, r);
    mm_journal_status status = wait_for(p, r, err);
    if (status == MM_JOURNAL_OK) {
        if (r->size > cap) status = MM_JOURNAL_TOO_SMALL;
        else { memcpy(buf, r->value, r->size); *used = r->size; }
    }
    release(r);
    return status;
}

mm_journal_status mm_nimble_exchange(void *ctx, const uint8_t *batch, size_t size,
                                    uint8_t *reply, size_t cap, size_t *used)
{
    mm_nimble_peer *p = ctx;
    if (used) *used = 0;
    if (!allowed(p) || !p->write_handle || !p->read_handle) return MM_JOURNAL_INVALID;
    uint16_t mtu = ble_att_mtu(p->conn);
    if (mtu < 23) return MM_JOURNAL_IO;
    uint8_t value[VALUE_CAP];
    size_t limit = (size_t)mtu - 3;
    mm_gatt_link link = {p, write_value, read_value, value, limit < sizeof value ? limit : sizeof value};
    return mm_gatt_exchange(&link, batch, size, reply, cap, used);
}
