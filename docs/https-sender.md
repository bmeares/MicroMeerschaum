# Sending a backlog over HTTPS

Build the ESP-IDF component with `CONFIG_MICROMEERSCHAUM_HTTPS=y`. Provision the receiver's node ID, HTTPS URL ending in `/micromeerschaum/v1/batch`, its scoped API key and the server's CA. A device using the ESP-IDF certificate bundle can pass a null CA pointer. Open the radio on the application's schedule. The following C example drains up to eight bounded batches over one client and closes the window even if a request fails.

```c
#include "micromeerschaum/https.h"

mm_journal_status upload(mm_journal *origin, uint64_t receiver_id, const char *url,
                         const char *api_key, const char *ca_pem,
                         uint8_t *buffer, size_t buffer_size)
{
    mm_https_client *client;
    mm_journal_status status = mm_https_open(&client, url, api_key, ca_pem, 15000);
    if (status != MM_JOURNAL_OK) return status;
    mm_send_result result;
    status = mm_send_window(origin, receiver_id, mm_https_exchange, client,
                            buffer, buffer_size, 8, &result);
    mm_https_close(client);
    if (status != MM_JOURNAL_OK) return status;
    return mm_journal_reclaim(origin);
}
```

The receipt confirms durable raw capture at the named destination. Each valid reply advances the persisted cursor. Losing a reply leaves the corresponding records available for retry on the next wake. A receiving node can acknowledge the accounted-for prefix even when sensor decoding is delayed. The configured buffer must fit at least the next complete record and its batch header. [Receipt format](../spec/protocol-v1.md#receipt)

For a complete first upload with the onboard temperature source, use the board and hosted receiver walkthrough. [Send the first reading](micropython.md#send-the-first-reading-to-a-hosted-pipe)

On the ESP32-S3 MicroPython image, configure Wi-Fi and a valid UTC clock before delivery. Open a session with the full ingest URL and a scoped API token. Supply a third argument with the PEM trust anchor for a private certificate authority. The session reuses its TLS connection across exchanges.

```python
import mm_https
from micromeerschaum import provision_peer

client = mm_https.open(url, token)
try:
    provision_peer("api:site", destination_id, client.exchange)
    ok, message = pipe.copy_to("api:site")
    pipe.reclaim()  # Make room for the next collection window.
    if not ok:
        raise OSError(message)
finally:
    client.close()
```

The local journal releases whole segments through the last verified receipt. If the transport fails partway through a copy, the next window retries the remaining records.
