// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/device_session.h"
#include "micromeerschaum/session_gatt.h"
#include "cJSON.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mm_actions actions;
static const mm_action *slots[3];
static mm_device_session serial, bluetooth;
static mm_device_session_config policy;
static mm_action_shell shell;
static char message[256];
static uint8_t address[6], own_type;
static const ble_uuid128_t management = BLE_UUID128_INIT(1,0,0x68,0xea,0x47,0x5e,0x68,0xbb,0x95,0x4f,0x71,0x97,0x70,0x5f,0x72,0x2f);

static bool usb_write(const char *p, size_t len, void *unused)
{
    (void)unused;
#ifdef CONFIG_MM_EXAMPLE_USB_JTAG
    return usb_serial_jtag_write_bytes(p, len, pdMS_TO_TICKS(1000)) == (int)len;
#else
    return uart_write_bytes(UART_NUM_0, p, len) == (int)len;
#endif
}

static bool uptime(const mm_action_call *call, mm_action_output *out, void *unused)
{
    (void)unused;
    if (call->argc) { mm_action_puts(out, "Usage: show uptime"); return false; }
    char value[40];
    snprintf(value, sizeof value, "%lld ms", (long long)(esp_timer_get_time() / 1000));
    mm_action_puts(out, value);
    return true;
}

static bool read_clock(const cJSON *request, cJSON *rows, mm_action_output *out, void *unused)
{
    (void)unused;
    if (!cJSON_GetObjectItemCaseSensitive(request, "limit")->valueint) return true;
    cJSON *row = cJSON_CreateObject();
    if (row && cJSON_AddNumberToObject(row, "microseconds", (double)esp_timer_get_time()) &&
        cJSON_AddItemToArray(rows, row)) return true;
    cJSON_Delete(row);
    mm_action_puts(out, "Could not allocate reading.");
    return false;
}

static bool action_allowed(const mm_action *a, void *unused)
{
    (void)unused;
    return !strcmp(a->name, "help") || !strcmp(a->name, "show actions") || !strcmp(a->name, "show uptime");
}

static bool op_allowed(const char *op, void *unused)
{
    (void)unused;
    return !strcmp(op, "actions") || !strcmp(op, "action") || !strcmp(op, "complete") ||
           !strcmp(op, "catalog") || !strcmp(op, "read");
}

static bool pipe_allowed(const mm_session_pipe *p, mm_session_access op, void *unused)
{
    (void)unused;
    return p->id == 1 && (op == MM_SESSION_DISCOVER || op == MM_SESSION_READ);
}

static bool enrolled(uint16_t conn, uint8_t type, const uint8_t identity[6], void *unused)
{
    (void)conn; (void)unused;
    return type == CONFIG_MM_EXAMPLE_PEER_TYPE && !memcmp(identity, address, 6);
}

static void advertise(void);
static int gap(struct ble_gap_event *event, void *unused)
{
    (void)unused;
    mm_session_gatt_event(event);
    if (event->type == BLE_GAP_EVENT_DISCONNECT ||
        (event->type == BLE_GAP_EVENT_CONNECT && event->connect.status)) advertise();
    if (event->type == BLE_GAP_EVENT_PASSKEY_ACTION && event->passkey.params.action == BLE_SM_IOACT_DISP) {
        struct ble_sm_io key = {.action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000};
        char text[48];
        int n = snprintf(text, sizeof text, "\r\nPairing passkey %06lu\r\n", (unsigned long)key.passkey);
        if (usb_write(text, (size_t)n, NULL)) ble_sm_inject_io(event->passkey.conn_handle, &key);
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = &management, .num_uuids128 = 1, .uuids128_is_complete = 1};
    struct ble_gap_adv_params params = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    if (ble_gap_adv_set_fields(&fields) || ble_gap_adv_start(own_type, NULL, BLE_HS_FOREVER, &params, gap, NULL))
        abort();
}
static void synced(void)
{
    if (ble_hs_id_infer_auto(0, &own_type)) abort();
    advertise();
}
static void host(void *unused) { (void)unused; nimble_port_run(); nimble_port_freertos_deinit(); }
void ble_store_config_init(void);

static void start_ble(void)
{
    const char *text = CONFIG_MM_EXAMPLE_PEER_ADDRESS;
    if (!*text) return;
    unsigned octet[6]; char tail;
    if (strlen(text) != 17 || sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c", &octet[5], &octet[4], &octet[3],
                                      &octet[2], &octet[1], &octet[0], &tail) != 6) abort();
    for (size_t i = 0; i < 6; i++) address[i] = (uint8_t)octet[i];
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = synced;
    ble_hs_cfg.sm_bonding = ble_hs_cfg.sm_mitm = ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_our_key_dist = ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (mm_session_gatt_register(&bluetooth, enrolled, NULL)) abort();
    nimble_port_freertos_init(host);
}

/* Buffer a partial reserved prefix before deciding whether a line belongs to
 * the human console. An oversized frame remains in the protocol reader until
 * LF; none of its contents can be submitted to the human dispatcher. */
static void console(void)
{
    const char prefix[] = MM_SESSION_PREFIX;
    size_t matched = 0;
    bool human = false, framed = false;
    while (shell.active) {
        char byte;
#ifdef CONFIG_MM_EXAMPLE_USB_JTAG
        int n = usb_serial_jtag_read_bytes(&byte, 1, portMAX_DELAY);
#else
        int n = uart_read_bytes(UART_NUM_0, &byte, 1, portMAX_DELAY);
#endif
        if (n != 1) continue;
        if (!human && !framed) {
            if (byte == prefix[matched]) {
                if (++matched == sizeof prefix - 1) {
                    framed = true;
                    if (!mm_device_session_feed(&serial, prefix, matched, usb_write, NULL)) break;
                }
                continue;
            }
            human = true;
            for (size_t i = 0; i < matched; i++) mm_action_shell_feed(&shell, (unsigned char)prefix[i]);
        }
        if (framed) {
            if (!mm_device_session_feed(&serial, &byte, 1, usb_write, NULL)) break;
        } else mm_action_shell_feed(&shell, (unsigned char)byte);
        if (byte == '\n' || (human && byte == '\r')) { matched = 0; framed = human = false; }
    }
}

void app_main(void)
{
#ifdef CONFIG_MM_EXAMPLE_USB_JTAG
    usb_serial_jtag_driver_config_t usb = {.tx_buffer_size = 1024, .rx_buffer_size = 1024};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
#else
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_baudrate(UART_NUM_0, 115200));
#endif
    static const mm_action clock_action = {"show uptime", "uptime", "Print uptime.", uptime, NULL};
    static const mm_session_pipe clock_pipe = {.id = 1, .connector = "sensor", .metric = "uptime",
        .instance = "clock:local", .read = read_clock};
    if (!mm_actions_init(&actions, slots, 3) || !mm_actions_register(&actions, &clock_action)) abort();
    policy = (mm_device_session_config){mm_actions_remote(&actions, action_allowed, NULL), MM_ACTION_QUOTED,
                                        &clock_pipe, 1, op_allowed, pipe_allowed, NULL};
    if (!mm_device_session_init(&serial, &policy) || !mm_device_session_init(&bluetooth, &policy)) abort();
    if (!mm_action_shell_init(&shell, mm_actions_local(&actions), MM_ACTION_QUOTED, message, sizeof message,
                              usb_write, NULL, true)) abort();
    start_ble();
    console();
}
