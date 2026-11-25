// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "micromeerschaum/gatt.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%d: %s\n",__LINE__,#x);exit(1); } } while(0)

typedef struct {
    mm_gatt_server server;
    unsigned accepted;
    bool offline;
    uint8_t batch[170], receipt[135];
} endpoint;

static mm_journal_status accept_batch(void *ctx, const uint8_t *data, size_t len,
                                      uint8_t *reply, size_t cap, size_t *used)
{
    endpoint *e = ctx;
    if (e->offline) return MM_JOURNAL_IO;
    CHECK(len == 170 && cap >= 135);
    for (size_t i=0;i<len;++i) CHECK(data[i]==(uint8_t)i);
    for (size_t i=0;i<135;++i) reply[i]=(uint8_t)(0xffu-i);
    *used=135; ++e->accepted; return MM_JOURNAL_OK;
}
static mm_journal_status write_value(void *ctx,const uint8_t *p,size_t n) {
    return mm_gatt_write(&((endpoint*)ctx)->server,p,n);
}
static mm_journal_status read_value(void *ctx,uint8_t *p,size_t n,size_t *used) {
    return mm_gatt_read(&((endpoint*)ctx)->server,p,n,used);
}
int main(void) {
    endpoint e={0};
    CHECK(mm_gatt_init(&e.server,e.batch,sizeof e.batch,e.receipt,sizeof e.receipt,accept_batch,&e)==MM_JOURNAL_OK);
    uint8_t batch[170],reply[135],att[20]; size_t used;
    for(size_t i=0;i<sizeof batch;++i)batch[i]=(uint8_t)i;
    mm_gatt_link link={.ctx=&e,.write=write_value,.read=read_value,.scratch=att,.value_cap=sizeof att};
    CHECK(mm_gatt_exchange(&link,batch,sizeof batch,reply,sizeof reply,&used)==MM_JOURNAL_OK);
    CHECK(used==135 && e.accepted==1);
    for(size_t i=0;i<used;++i)CHECK(reply[i]==(uint8_t)(0xffu-i));
    e.offline=true;
    CHECK(mm_gatt_exchange(&link,batch,sizeof batch,reply,sizeof reply,&used)==MM_JOURNAL_IO && used==0);
    CHECK(mm_gatt_read(&e.server,att,sizeof att,&used)!=MM_JOURNAL_OK && used==0);
    uint8_t partial[]={1,0,0,0,0,170,0,0,0,0,1};
    CHECK(mm_gatt_write(&e.server,partial,sizeof partial)==MM_JOURNAL_OK);
    mm_gatt_reset(&e.server);
    partial[1]=2;
    CHECK(mm_gatt_write(&e.server,partial,sizeof partial)==MM_JOURNAL_INVALID);
    partial[1]=0;partial[5]=171;
    CHECK(mm_gatt_write(&e.server,partial,sizeof partial)==MM_JOURNAL_TOO_SMALL);
    return 0;
}
