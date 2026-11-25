// SPDX-License-Identifier: Apache-2.0
#ifndef MICROMEERSCHAUM_GATT_H
#define MICROMEERSCHAUM_GATT_H
#include "micromeerschaum/journal.h"
#ifdef __cplusplus
extern "C" {
#endif

#define MM_GATT_SERVICE_UUID "d9da0001-5af1-49e5-9d3a-825119b55722"
#define MM_GATT_WRITE_UUID   "d9da0002-5af1-49e5-9d3a-825119b55722"
#define MM_GATT_READ_UUID    "d9da0003-5af1-49e5-9d3a-825119b55722"
#define MM_GATT_HEADER 9u

/* One state per authorized connection. Reset on disconnect or identity change. */
typedef mm_journal_status (*mm_gatt_commit)(void *, const uint8_t *, size_t,
                                            uint8_t *, size_t, size_t *);
typedef struct {
    uint8_t *batch, *reply;
    size_t batch_cap, reply_cap;
    size_t total, received, reply_len, read_at, read_limit;
    mm_gatt_commit commit;
    void *ctx;
} mm_gatt_server;
mm_journal_status mm_gatt_init(mm_gatt_server *, uint8_t *batch, size_t batch_cap,
                              uint8_t *reply, size_t reply_cap, mm_gatt_commit, void *);
void mm_gatt_reset(mm_gatt_server *);
mm_journal_status mm_gatt_write(mm_gatt_server *, const uint8_t *, size_t);
mm_journal_status mm_gatt_read(mm_gatt_server *, uint8_t *, size_t, size_t *);

/* Writes complete only after the ATT write response. value_cap is the smaller
 * of the negotiated ATT MTU minus three and the characteristic's value limit. */
typedef struct {
    void *ctx;
    mm_journal_status (*write)(void *, const uint8_t *, size_t);
    mm_journal_status (*read)(void *, uint8_t *, size_t, size_t *);
    uint8_t *scratch;
    size_t value_cap;
} mm_gatt_link;
mm_journal_status mm_gatt_exchange(void *link, const uint8_t *data, size_t len,
                                  uint8_t *reply, size_t cap, size_t *used);
#ifdef __cplusplus
}
#endif
#endif
