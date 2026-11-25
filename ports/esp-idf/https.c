// SPDX-License-Identifier: Apache-2.0
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_client.h"
#include "sdkconfig.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "micromeerschaum/https.h"

struct mm_https_client {
    esp_http_client_handle_t http;
    uint8_t *reply;
    size_t cap, used;
    bool overflow;
};

static esp_err_t collect_body(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    mm_https_client *client = event->user_data;
    if (event->data_len < 0 || (size_t)event->data_len > client->cap - client->used) {
        client->overflow = true;
        return ESP_FAIL;
    }
    if (event->data_len && !client->overflow) {
        memcpy(client->reply + client->used, event->data, (size_t)event->data_len);
        client->used += (size_t)event->data_len;
    }
    return ESP_OK;
}

mm_journal_status mm_https_open(mm_https_client **out, const char *url, const char *token,
                               const char *ca_pem, int timeout_ms)
{
    if (!out) return MM_JOURNAL_INVALID;
    *out = NULL;
    if (!url || strncmp(url, "https://", 8) || !url[8] || !token || !*token || timeout_ms <= 0)
        return MM_JOURNAL_INVALID;
    size_t token_len = strlen(token);
    if (token_len > 4096 || strpbrk(token, "\r\n")) return MM_JOURNAL_INVALID;
    mm_https_client *client = calloc(1, sizeof *client);
    if (!client) return MM_JOURNAL_NOMEM;
    esp_http_client_config_t config = {
        .url = url, .method = HTTP_METHOD_POST, .cert_pem = ca_pem, .timeout_ms = timeout_ms,
        .disable_auto_redirect = true, .max_authorization_retries = -1, .keep_alive_enable = true,
        .event_handler = collect_body, .user_data = client,
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (!ca_pem) config.crt_bundle_attach = esp_crt_bundle_attach;
#else
    if (!ca_pem) { free(client); return MM_JOURNAL_INVALID; }
#endif
    client->http = esp_http_client_init(&config);
    if (!client->http) { free(client); return MM_JOURNAL_IO; }

    char *auth = malloc(token_len + 8);
    if (!auth) { mm_https_close(client); return MM_JOURNAL_NOMEM; }
    memcpy(auth, "Bearer ", 7);
    memcpy(auth + 7, token, token_len + 1);
    esp_err_t rc = esp_http_client_set_header(client->http, "Authorization", auth);
    free(auth); /* IDF copies header values. */
    if (rc != ESP_OK ||
        esp_http_client_set_header(client->http, "Content-Type", "application/octet-stream") != ESP_OK ||
        esp_http_client_set_header(client->http, "Accept", "application/octet-stream") != ESP_OK) {
        mm_https_close(client);
        return MM_JOURNAL_IO;
    }
    *out = client;
    return MM_JOURNAL_OK;
}

mm_journal_status mm_https_exchange(void *ctx, const uint8_t *data, size_t len,
                                   uint8_t *reply, size_t cap, size_t *used)
{
    if (!used) return MM_JOURNAL_INVALID;
    *used = 0;
    mm_https_client *client = ctx;
    if (!client || !data || !len || len > INT_MAX || !reply || !cap) return MM_JOURNAL_INVALID;
    client->reply = reply;
    client->cap = cap;
    client->used = 0;
    client->overflow = false;
    if (esp_http_client_set_post_field(client->http, (const char *)data, (int)len) != ESP_OK)
        return MM_JOURNAL_IO;
    esp_err_t rc = esp_http_client_perform(client->http);
    client->reply = NULL;
    if (rc != ESP_OK || esp_http_client_get_status_code(client->http) != 200 ||
        !esp_http_client_is_complete_data_received(client->http) || client->overflow || !client->used) {
        esp_http_client_close(client->http);
        return client->overflow ? MM_JOURNAL_TOO_SMALL : MM_JOURNAL_IO;
    }
    *used = client->used;
    return MM_JOURNAL_OK;
}

void mm_https_close(mm_https_client *client)
{
    if (!client) return;
    esp_http_client_cleanup(client->http);
    free(client);
}
