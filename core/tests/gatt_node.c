// SPDX-License-Identifier: Apache-2.0
/* Test node. stdin/stdout carry one ATT characteristic operation per line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "micromeerschaum/document.h"
#include "micromeerschaum/gatt.h"
#include "micromeerschaum/sender.h"

#define OK(x) do { if ((x) != MM_JOURNAL_OK) { fprintf(stderr,"node line %d\n",__LINE__); return 1; } } while (0)
static void hex(const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x", data[i]);
    puts("");
}
static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = strcspn(s, "\r\n");
    if (n % 2 || n / 2 > cap) return 0;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned byte;
        if (sscanf(s + 2*i, "%2x", &byte) != 1) return 0;
        out[i] = (uint8_t)byte;
    }
    return n / 2;
}
static mm_journal_status tx(void *ctx, const uint8_t *data, size_t n)
{
    (void)ctx;
    putchar('W'); hex(data,n);
    char line[16];
    return fgets(line,sizeof line,stdin) && !strcmp(line,"OK\n") ? MM_JOURNAL_OK : MM_JOURNAL_IO;
}
static mm_journal_status rx(void *ctx, uint8_t *data, size_t cap, size_t *n)
{
    (void)ctx;
    puts("R");
    char line[64];
    *n = fgets(line,sizeof line,stdin) && *line == 'V' ? unhex(line+1, data, cap) : 0;
    return *n ? MM_JOURNAL_OK : MM_JOURNAL_IO;
}
static mm_journal_status receive(void *ctx, const uint8_t *buf, size_t n, uint8_t *reply,
                                 size_t cap, size_t *used)
{
    mm_journal *j = ctx;
    uint64_t sender = mm_journal_config_get(j)->role == MM_CUSTODY ? 1 : 2;
    return mm_peer_receive(j, sender, buf, n, reply, cap, used);
}
static int serve(mm_journal *j, bool drop)
{
    mm_gatt_server state;
    uint8_t batch[512], reply[MM_RECEIPT_MAX], fragment[20];
    OK(mm_gatt_init(&state,batch,sizeof batch,reply,sizeof reply,receive,j));
    char line[64];
    while (fgets(line,sizeof line,stdin)) {
        if (*line == 'W') {
            size_t n = unhex(line+1,fragment,sizeof fragment);
            OK(mm_gatt_write(&state,fragment,n));
            puts("OK");
        } else if (*line == 'R') {
            if (drop) { puts("!"); break; }
            size_t n;
            OK(mm_gatt_read(&state,fragment,sizeof fragment,&n));
            putchar('V'); hex(fragment,n);
        } else return 1;
    }
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 4) return 1;
    setvbuf(stdout,NULL,_IONBF,0);
    bool origin = !strcmp(argv[1],"origin"), dest = !strcmp(argv[1],"dest");
    const char *op = argv[3];
    const char key[] = "sensor\x1ftemp\x1f";
    mm_journal_config c = {{1,8,key,sizeof key-1}, origin ? MM_JOURNAL_ORIGIN : MM_JOURNAL_RECEIVER,
                           dest ? MM_DESTINATION : MM_CUSTODY, dest ? 9 : 2, 9, 0};
    mm_volume_options opt = {4,512,MM_KEEP_OLD};
    mm_journal *j;
    OK(mm_journal_open_volume(&j,argv[2],&c,&opt,true));
    int result = 0;
    if (!strcmp(op,"seed")) {
        uint8_t doc[128];
        size_t n;
        mm_field f = {{"tick",4},{MM_VALUE_INT,{.integer=41}}};
        if (mm_document_encode(doc,sizeof doc,(mm_row){&f,1},&n) != MM_OK) return 1;
        uint64_t seq;
        OK(mm_journal_append(j,MM_KIND_DATA,7,1,doc,(uint32_t)n,&seq));
    } else if (!strcmp(op,"status")) {
        uint8_t frame[512];
        size_t n;
        printf("%" PRIu64 " %" PRIu64 " ",mm_journal_prefix(j),mm_journal_confirmed(j));
        if (mm_journal_read(j,1,frame,sizeof frame,&n) == MM_JOURNAL_OK) hex(frame,n);
        else puts("-");
    } else if (!strcmp(op,"send")) {
        uint8_t fragment[20], batch[512];
        mm_gatt_link link = {NULL,tx,rx,fragment,sizeof fragment};
        mm_send_result progress;
        mm_journal_status st = mm_send_window(j,origin ? 2 : 9,mm_gatt_exchange,&link,batch,
                                               sizeof batch,1,&progress);
        if (st == MM_JOURNAL_IO) result = 3;
        else { OK(st); OK(mm_journal_reclaim(j)); }
    } else result = serve(j,!strcmp(op,"drop"));
    mm_journal_close(j);
    return result;
}
