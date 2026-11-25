// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/session_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include <string.h>

#define UUID(n) BLE_UUID128_INIT(n,0x00,0x68,0xea,0x47,0x5e,0x68,0xbb,0x95,0x4f,0x71,0x97,0x70,0x5f,0x72,0x2f)
static const ble_uuid128_t service_id = UUID(1), rx_id = UUID(2), tx_id = UUID(3);
static uint16_t tx_handle;
static mm_device_session *session;
static mm_session_peer_fn enroll;
static void *enroll_ctx;
static TaskHandle_t worker;
static struct ble_npl_event notification;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t owner = BLE_HS_CONN_HANDLE_NONE, job_owner;
static uint64_t generation, job_generation;
static ble_addr_t identity;
static bool busy, discard;
static size_t length;
static char request[MM_SESSION_FRAME_MAX];

/* Only the worker writes these fields before posting to the host queue. The
 * host signals completion before the worker reuses the span. */
static const char *tx_data;
static size_t tx_remaining, tx_sent;
static int tx_status;

static bool enrolled(uint16_t conn, ble_addr_t *id)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) || !desc.sec_state.encrypted || !desc.sec_state.authenticated ||
        !desc.sec_state.bonded || !enroll(conn, desc.peer_id_addr.type, desc.peer_id_addr.val, enroll_ctx))
        return false;
    *id = desc.peer_id_addr;
    return true;
}

static void revoke(uint16_t conn)
{
    portENTER_CRITICAL(&lock);
    if (owner == conn) {
        owner = BLE_HS_CONN_HANDLE_NONE;
        generation++;
        discard = false;
        if (!busy) length = 0;
    }
    portEXIT_CRITICAL(&lock);
}

static bool current(uint16_t conn, uint64_t gen)
{
    ble_addr_t id;
    if (!enrolled(conn, &id)) return false;
    portENTER_CRITICAL(&lock);
    bool ok = owner == conn && gen == generation && !ble_addr_cmp(&identity, &id);
    portEXIT_CRITICAL(&lock);
    return ok;
}

void mm_session_gatt_event(const struct ble_gap_event *e)
{
    if (!session || !e) return;
    if (e->type == BLE_GAP_EVENT_DISCONNECT) { revoke(e->disconnect.conn.conn_handle); return; }
    if (e->type == BLE_GAP_EVENT_ENC_CHANGE) {
        ble_addr_t id;
        if (!enrolled(e->enc_change.conn_handle, &id)) revoke(e->enc_change.conn_handle);
        return;
    }
    if (e->type != BLE_GAP_EVENT_SUBSCRIBE || e->subscribe.attr_handle != tx_handle) return;
    uint16_t conn = e->subscribe.conn_handle;
    ble_addr_t id;
    if (!e->subscribe.cur_notify || !enrolled(conn, &id)) { revoke(conn); return; }
    portENTER_CRITICAL(&lock);
    if (owner == BLE_HS_CONN_HANDLE_NONE) {
        owner = conn;
        identity = id;
        generation++;
    }
    portEXIT_CRITICAL(&lock);
}

static int receive(uint16_t conn, uint16_t handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)handle; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    ble_addr_t id;
    if (!enrolled(conn, &id)) { revoke(conn); return BLE_ATT_ERR_INSUFFICIENT_AUTHOR; }
    /* A fragment contains bytes from one frame. Keep all application work
     * off the NimBLE host stack, including JSON parsing and Pipe callbacks. */
    uint8_t fragment[BLE_ATT_MTU_MAX];
    size_t n = OS_MBUF_PKTLEN(ctxt->om);
    if (!n || n > sizeof fragment || os_mbuf_copydata(ctxt->om, 0, (int)n, fragment))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    const uint8_t *lf = memchr(fragment, '\n', n);
    if (lf && lf != fragment + n - 1) {
        /* A nonconforming writer must never resume an earlier partial frame. */
        revoke(conn);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    int result = 0;
    bool start = false;
    portENTER_CRITICAL(&lock);
    if (owner != conn || ble_addr_cmp(&identity, &id)) result = BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    else if (busy) result = BLE_ATT_ERR_INSUFFICIENT_RES;
    else {
        if (n > sizeof request - length) discard = true;
        if (!discard) { memcpy(request + length, fragment, n); length += n; }
        if (lf) {
            if (discard) { discard = false; length = 0; }
            else {
                job_owner = owner;
                job_generation = generation;
                busy = start = true;
            }
        }
    }
    portEXIT_CRITICAL(&lock);
    if (start) xTaskNotifyGive(worker);
    return result;
}

static const struct ble_gatt_chr_def chars[] = {
    {.uuid = &rx_id.u, .access_cb = receive,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN},
    {.uuid = &tx_id.u, .access_cb = receive, .val_handle = &tx_handle, .flags = BLE_GATT_CHR_F_NOTIFY},
    {0}
};
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &service_id.u, .characteristics = chars}, {0}
};

/* Recheck the generation and the resolved bonded identity on the NimBLE
 * host before each fragment. A recycled handle cannot inherit a reply. */
static void send_on_host(struct ble_npl_event *event)
{
    (void)event;
    tx_sent = 0;
    if (!current(job_owner, job_generation)) tx_status = BLE_HS_ENOTCONN;
    else if (!tx_data) tx_status = ble_gap_terminate(job_owner, BLE_ERR_REM_USER_CONN_TERM);
    else {
        uint16_t mtu = ble_att_mtu(job_owner);
        size_t n = mtu > 3 ? (size_t)mtu - 3u : 20u;
        if (n > tx_remaining) n = tx_remaining;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(tx_data, (uint16_t)n);
        tx_status = om ? ble_gatts_notify_custom(job_owner, tx_handle, om) : BLE_HS_ENOMEM;
        if (!tx_status) tx_sent = n;
    }
    xTaskNotifyGive(worker);
}

static void submit_fragment(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &notification);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static bool emit(const char *bytes, size_t len, void *unused)
{
    (void)unused;
    tx_data = bytes;
    tx_remaining = len;
    unsigned retries = 0;
    while (tx_remaining) {
        submit_fragment();
        if (tx_status == BLE_HS_ENOMEM || tx_status == BLE_HS_EAGAIN || tx_status == BLE_HS_EBUSY) {
            if (++retries > 100) break;
            vTaskDelay(pdMS_TO_TICKS(10) + 1);
            continue;
        }
        if (tx_status) break;
        tx_data += tx_sent;
        tx_remaining -= tx_sent;
    }
    if (!tx_remaining) return true;
    tx_data = NULL;
    submit_fragment();
    return false;
}

static void run(void *unused)
{
    (void)unused;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (current(job_owner, job_generation))
            mm_device_session_line(session, request, length, emit, NULL);
        portENTER_CRITICAL(&lock);
        length = 0;
        discard = busy = false;
        portEXIT_CRITICAL(&lock);
    }
}

int mm_session_gatt_register(mm_device_session *s, mm_session_peer_fn allowed, void *user)
{
    if (!s || !s->config || !allowed || session) return BLE_HS_EINVAL;
    session = s;
    enroll = allowed;
    enroll_ctx = user;
    ble_npl_event_init(&notification, send_on_host, NULL);
    int rc = xTaskCreate(run, "mm_session", CONFIG_MICROMEERSCHAUM_SESSION_STACK, NULL,
                         tskIDLE_PRIORITY + 2, &worker) == pdPASS ? 0 : BLE_HS_ENOMEM;
    if (!rc) rc = ble_gatts_count_cfg(services);
    if (!rc) rc = ble_gatts_add_svcs(services);
    if (rc) {
        if (worker) { vTaskDelete(worker); worker = NULL; }
        ble_npl_event_deinit(&notification);
        session = NULL;
    }
    return rc;
}
