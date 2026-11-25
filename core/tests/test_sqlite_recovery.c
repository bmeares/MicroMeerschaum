// SPDX-License-Identifier: Apache-2.0
#define _LARGEFILE64_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "micromeerschaum/sqlite.h"

/* Interrupt or fail actual host-VFS writes and barriers (GNU link wrapping).
 * Each child opens its own database connection. The parent performs recovery
 * and reads the result exclusively through the native Pipe/instance APIs. */
static int barrier, cut, no_space;
int __real_fsync(int);
ssize_t __real_pwrite64(int,const void *,size_t,off64_t);
int __wrap_fsync(int fd)
{
    if (barrier && !--barrier) {
        if (cut) _exit(42);
        errno=EIO;
        return -1;
    }
    return __real_fsync(fd);
}

ssize_t __wrap_pwrite64(int fd, const void *data, size_t n, off64_t off)
{
    if (no_space && !--no_space) { errno=ENOSPC; return -1; }
    return __real_pwrite64(fd,data,n,off);
}

static const char key[]="sensor\x1f" "reading\x1f";
static const mm_journal_config config={{1,2,key,sizeof key-1},MM_JOURNAL_ORIGIN,MM_DESTINATION,3,3,0};
static const mm_sqlite_column cols[]={{"tick",MM_VALUE_INT,true}};
static const mm_sqlite_definition def={cols,1,0,false};

static mm_sync_result write_group(mm_sqlite *db)
{
    const mm_field a={{"tick",4},{MM_VALUE_INT,{.integer=1}}}, b={{"tick",4},{MM_VALUE_INT,{.integer=2}}};
    mm_row rows[]={{&a,1},{&b,1}};
    uint8_t buf[80];
    mm_pipe p;
    assert(mm_pipe_open(&p,"sensor","reading",NULL,mm_sqlite_target(db),buf,sizeof buf)==MM_JOURNAL_OK);
    return mm_pipe_sync_rows(&p,rows,2,3,4);
}

static int one_case(int nth, bool crash, bool full, bool receipt)
{
    char dir[]="/tmp/mm-sql-recovery-XXXXXX", path[120], journal[130];
    assert(mkdtemp(dir));
    snprintf(path,sizeof path,"%s/data.db",dir);
    mm_sqlite *db;
    assert(mm_sqlite_open(&db,path,&config,&def)==MM_JOURNAL_OK);
    if (receipt) assert(write_group(db).confirmed==2);
    mm_sqlite_close(db);

    pid_t child=fork();
    assert(child >= 0);
    if (!child) {
        assert(mm_sqlite_open(&db,path,&config,NULL)==MM_JOURNAL_OK);
        barrier=full ? 0 : nth;
        no_space=full ? nth : 0;
        cut=crash;
        mm_journal_status st;
        if (receipt) {
            mm_receipt r={config.stream,config.role,config.issuer,config.destination,2};
            st=mm_sqlite_confirm(db,3,&r);
        } else {
            mm_sync_result result=write_group(db);
            st=result.status;
            if (st==MM_JOURNAL_UNCERTAIN)
                assert(result.uncertain_first==1 && result.uncertain_count==2 && !result.confirmed);
        }
        _exit(st==MM_JOURNAL_OK ? 0 : 25);
    }
    int status;
    assert(waitpid(child,&status,0)==child && WIFEXITED(status));
    int code=WEXITSTATUS(status);
    assert(code==0 || code==25 || code==42);
    assert(mm_sqlite_open(&db,path,&config,NULL)==MM_JOURNAL_OK);
    uint64_t high,confirmed;
    assert(mm_sqlite_positions(db,&high,&confirmed)==MM_JOURNAL_OK);
    assert(high==0 || high==2);
    assert(confirmed==0 || confirmed==2);
    if (receipt) assert(high==2);
    else assert(confirmed==0);
    if (code==0) assert(receipt ? confirmed==2 : high==2);

    mm_sqlite_query q={.limit=3};
    mm_sqlite_cursor *c;
    mm_row row;
    assert(mm_sqlite_get_docs(db,&q,&c)==MM_JOURNAL_OK);
    uint8_t data[128];
    size_t n;
    for (uint64_t i=1;i<=high;++i) {
        assert(mm_sqlite_next(c,&row)==MM_JOURNAL_OK && row.fields[0].value.as.integer==(int64_t)i);
        mm_journal_status st=mm_sqlite_read(db,i,data,sizeof data,&n);
        assert(st==(i<=confirmed ? MM_JOURNAL_NOT_FOUND : MM_JOURNAL_OK));
    }
    assert(mm_sqlite_next(c,&row)==MM_JOURNAL_NOT_FOUND);
    mm_sqlite_cursor_close(c);
    assert(mm_sqlite_read(db,high+1,data,sizeof data,&n)==MM_JOURNAL_NOT_FOUND);
    assert(write_group(db).confirmed==2);
    assert(mm_sqlite_read(db,high+2,data,sizeof data,&n)==MM_JOURNAL_OK);
    mm_sqlite_close(db);
    unlink(path);
    snprintf(journal,sizeof journal,"%s-journal",path);
    unlink(journal);
    assert(!rmdir(dir));
    return code;
}

int main(void)
{
    for (int confirm=0;confirm<=1;++confirm) {
        int interrupted=0,failed=0,full=0;
        for (int i=1;i<=5;++i) {
            interrupted += one_case(i,true,false,confirm)!=0;
            failed += one_case(i,false,false,confirm)!=0;
            full += one_case(i,false,true,confirm)!=0;
        }
        assert(interrupted>=2 && failed>=2 && full>=1);
    }
}
