// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "micromeerschaum/sender.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

typedef struct {
    mm_journal *peer;
    bool lose_reply;
    unsigned calls;
} test_link;

static mm_journal_status exchange(void *context, const uint8_t *data, size_t len,
                                  uint8_t *reply, size_t cap, size_t *used)
{
    test_link *link = context;
    ++link->calls;
    mm_journal_status st = mm_peer_receive(link->peer, 11, data, len, reply, cap, used);
    if (link->lose_reply && st == MM_JOURNAL_OK) {
        link->lose_reply = false;
        *used = 0;
        return MM_JOURNAL_IO;
    }
    return st;
}
static void cleanup(const char *path) {
    DIR *dir = opendir(path);
    CHECK(dir != NULL);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        CHECK(unlinkat(dirfd(dir),entry->d_name,0) == 0);
    }
    CHECK(closedir(dir) == 0 && rmdir(path) == 0);
}
int main(void) {
    char a[]="/tmp/mm-sender-a-XXXXXX", b[]="/tmp/mm-sender-b-XXXXXX";
    CHECK(mkdtemp(a) && mkdtemp(b));
    static const char key[]="sensor\x1f" "temp\x1f";
    mm_journal_config config = {.stream={11,1,key,sizeof key-1}, .issuer=22, .destination=22,
                                .role=MM_DESTINATION, .mode=MM_JOURNAL_ORIGIN};
    mm_journal *origin, *peer;
    CHECK(mm_journal_open(&origin,a,&config,true)==MM_JOURNAL_OK);
    config.mode=MM_JOURNAL_RECEIVER;
    CHECK(mm_journal_open(&peer,b,&config,true)==MM_JOURNAL_OK);
    uint64_t seq;
    for (unsigned i=1;i<=3;++i)
        CHECK(mm_journal_append(origin,MM_KIND_DATA,i,1,"x",1,&seq)==MM_JOURNAL_OK);
    test_link link={peer,true,0};
    uint8_t buf[96]; /* Fits only one record. */
    mm_send_result result;
    CHECK(mm_send_window(origin,22,exchange,&link,buf,sizeof buf,1,&result)==MM_JOURNAL_IO);
    CHECK(mm_journal_prefix(peer)==1 && mm_journal_confirmed(origin)==0 && result.batches==0);
    CHECK(mm_send_window(origin,22,exchange,&link,buf,sizeof buf,2,&result)==MM_JOURNAL_OK);
    CHECK(result.batches==2 && result.through==2 && mm_journal_confirmed(origin)==2);
    CHECK(mm_send_window(origin,22,exchange,&link,buf,sizeof buf,3,&result)==MM_JOURNAL_OK);
    CHECK(result.batches==1 && result.through==3 && mm_journal_prefix(peer)==3);
    CHECK(mm_send_window(origin,22,exchange,&link,buf,sizeof buf,3,&result)==MM_JOURNAL_OK);
    CHECK(result.batches==0 && result.through==3 && link.calls==4);
    mm_journal_close(origin); mm_journal_close(peer);
    cleanup(a); cleanup(b); return 0;
}
