// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_HTTPS_H
#define MICROMEERSCHAUM_HTTPS_H

#include "micromeerschaum/sender.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP-IDF with CONFIG_MICROMEERSCHAUM_HTTPS enabled. The application owns
 * network setup and securely provisions the URL, receiver identity and token.
 * Keep CA PEM (if supplied) alive until close. A NULL CA selects the IDF bundle. */
typedef struct mm_https_client mm_https_client;
mm_journal_status mm_https_open(mm_https_client **out, const char *url, const char *token,
                               const char *ca_pem, int timeout_ms);
mm_journal_status mm_https_exchange(void *client, const uint8_t *data, size_t len,
                                   uint8_t *reply, size_t cap, size_t *used);
void mm_https_close(mm_https_client *client);

#ifdef __cplusplus
}
#endif
#endif
