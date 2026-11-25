// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "micromeerschaum/journal_pipe.h"
#include "micromeerschaum/pipe_actions.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(1); } } while (0)

static mm_journal_status fetch(void *ctx, const mm_pipe *pipe, size_t index, mm_row *row, bool *done)
{
    (void)ctx;
    (void)pipe;
    static const mm_field sample = {{"tick", 4}, {MM_VALUE_INT, {.integer = 12}}};
    *done = index == 1;
    *row = (mm_row){&sample, 1};
    return MM_JOURNAL_OK;
}

static mm_journal_status push(void *ctx, const mm_sample *rows, size_t n, uint64_t *seq)
{
    (void)ctx; (void)rows; (void)n; (void)seq;
    CHECK(false); /* A push-only target may never be invoked by sync pipes. */
    return MM_JOURNAL_INVALID;
}

typedef struct {
    mm_journal *peer;
    bool lose_receipt;
    unsigned deliveries;
} link_state;

static mm_journal_status exchange(void *ctx, const uint8_t *data, size_t len,
                                  uint8_t *reply, size_t cap, size_t *used)
{
    link_state *link = ctx;
    mm_journal_status st = mm_peer_receive(link->peer, 11, data, len, reply, cap, used);
    if (st == MM_JOURNAL_OK && link->lose_receipt) return MM_JOURNAL_IO;
    return st;
}

static mm_journal_status deliver(const mm_action_pipe *p, mm_send_result *result, void *ctx)
{
    link_state *link = ctx;
    link->deliveries++;
    uint8_t batch[256];
    return mm_send_window(p->user, 22, exchange, link, batch, sizeof batch, 2, result);
}

typedef struct { bool list_push, sync, copy; } permission;

static bool allow_action(const mm_action *a, void *ctx)
{
    (void)a;
    return ctx != NULL;
}

static bool allow_pipe(const mm_action_view *v, const mm_action_pipe *p, mm_pipe_action_kind op,
                       const mm_action_destination *dest, void *ctx)
{
    (void)ctx;
    if (v->local) return true;
    const permission *session = v->user;
    if (op == MM_PIPE_SHOW) return p->pipe->fetch || session->list_push;
    return op == MM_PIPE_SYNC ? session->sync : session->copy && dest && !strcmp(dest->name, "peer:lab");
}

static uint64_t clock_us(void *ctx)
{
    (void)ctx;
    return 777;
}

static char message[384];
static bool run(mm_action_view v, const char *line)
{
    static mm_action_workspace work;
    mm_action_output out;
    mm_action_output_init(&out, message, sizeof message);
    bool ok = mm_actions_dispatch(&v, line, strlen(line), MM_ACTION_QUOTED, &work, &out);
    CHECK(!out.truncated);
    return ok;
}

int main(void)
{
    char dir[] = "/tmp/mm-pipe-actions-XXXXXX", opath[128], rpath[128];
    CHECK(mkdtemp(dir));
    snprintf(opath, sizeof opath, "%s/origin", dir);
    snprintf(rpath, sizeof rpath, "%s/peer", dir);
    static const char key[] = "plugin:sample\x1f" "daily\x1f";
    mm_journal_config journal = {{11, 1, key, sizeof key - 1}, MM_JOURNAL_ORIGIN, MM_DESTINATION, 22, 22, 0};
    mm_volume_options options = {4, 1024, MM_KEEP_OLD};
    mm_journal *origin, *peer;
    CHECK(mm_journal_open_volume(&origin, opath, &journal, &options, true) == MM_JOURNAL_OK);
    journal.mode = MM_JOURNAL_RECEIVER;
    CHECK(mm_journal_open_volume(&peer, rpath, &journal, &options, true) == MM_JOURNAL_OK);
    mm_pipe sensor, push_only;
    uint8_t scratch[64];
    CHECK(mm_pipe_open(&sensor, "plugin:sample", "daily", NULL, mm_pipe_journal_target(origin),
                       scratch, sizeof scratch) == MM_JOURNAL_OK);
    CHECK(mm_pipe_bind_source(&sensor, "sample", fetch, NULL) == MM_JOURNAL_OK);
    static const char other[] = "driver\x1f" "daily\x1f" "north";
    mm_pipe_target target = {{11, 1, other, sizeof other - 1}, push, NULL};
    CHECK(mm_pipe_open(&push_only, "driver", "daily", "north", target, NULL, 0) == MM_JOURNAL_OK);
    const mm_action_pipe published[] = {{&sensor, "journal:local", origin}, {&push_only, "journal:other", NULL}};
    link_state link = {peer, false, 0};
    const mm_action_destination destination = {"peer:lab", deliver, &link};
    mm_pipe_action_config cfg = {published, 2, &destination, 1, allow_pipe, clock_us, 12, NULL};
    mm_actions registry;
    mm_pipe_actions bindings;
    const mm_action *slots[5];
    CHECK(mm_actions_init(&registry, slots, 4));
    CHECK(!mm_pipe_actions_register(&registry, &bindings, &cfg) && registry.count == 2);
    CHECK(mm_actions_init(&registry, slots, 5));
    mm_action_pipe duplicate[] = {published[0], published[0]};
    mm_pipe_action_config invalid = cfg;
    invalid.pipes = duplicate;
    CHECK(!mm_pipe_actions_register(&registry, &bindings, &invalid) && registry.count == 2);
    CHECK(mm_pipe_actions_register(&registry, &bindings, &cfg));
    CHECK(!mm_pipe_actions_register(&registry, &bindings, &cfg));
    mm_action_view local = mm_actions_local(&registry);
    permission session = {false, false, false};
    mm_action_view remote = mm_actions_remote(&registry, allow_action, &session);
    CHECK(run(local, "show pipes") && strstr(message, "plugin:sample") && strstr(message, "driver"));
    CHECK(run(remote, "show pipes") && strstr(message, "plugin:sample") && !strstr(message, "driver"));
    CHECK(!run(remote, "sync pipes") && mm_journal_high(origin) == 0);
    CHECK(!run(remote, "copy pipes peer:lab") && !link.deliveries);
    CHECK(!run(local, "sync pipes -m daily") && mm_journal_high(origin) == 0);
    CHECK(run(local, "show pipes -l None") && !strstr(message, "driver"));
    CHECK(run(local, "show pipes --location-keys ''") && !strstr(message, "driver"));
    CHECK(run(local, "show pipes --mrsm-instance journal:other") && !strstr(message, "plugin:sample"));
    CHECK(run(local, "show pipes -c missing") && strstr(message, "No matching"));
    CHECK(!run(local, "sync pipes -c missing"));
    CHECK(!run(local, "copy pipes -c plugin:sample"));
    CHECK(!run(local, "copy pipes peer:unconfigured -c plugin:sample") && !link.deliveries);
    CHECK(!run(local, "sync pipes --unsupported"));
    CHECK(!run(local, "sync pipes -m"));
    CHECK(!run(local, "sync pipes -c -m"));
    CHECK(!run(local, "sync pipes -m daily --metric-keys daily"));
    CHECK(!run(local, "sync pipes extra"));
    CHECK(!run(local, "sync pipes -l north") && mm_journal_high(origin) == 0);
    CHECK(run(local, "sync pipes") && strstr(message, "1 row confirmed"));
    CHECK(mm_journal_high(origin) == 1 && !mm_journal_high(peer));
    session.sync = true;
    CHECK(run(remote, "sync pipes -l None --connector-keys plugin:sample"));
    CHECK(mm_journal_high(origin) == 2);
    session.copy = true;
    link.lose_receipt = true;
    CHECK(!run(remote, "copy pipes -i journal:local peer:lab") && link.deliveries == 1);
    CHECK(mm_journal_confirmed(origin) == 0 && mm_journal_high(peer) == 2);
    link.lose_receipt = false;
    CHECK(run(remote, "copy pipes peer:lab -i journal:local") && link.deliveries == 2);
    CHECK(strstr(message, "confirmed through 2") && mm_journal_confirmed(origin) == 2);
    CHECK(mm_journal_high(peer) == 2);
    mm_journal_close(peer);
    CHECK(mm_journal_open_volume(&peer, rpath, &journal, &options, false) == MM_JOURNAL_OK);
    uint8_t buf[96];
    size_t n;
    mm_record rec;
    mm_field field;
    CHECK(mm_journal_read(peer, 1, buf, sizeof buf, &n) == MM_JOURNAL_OK);
    CHECK(mm_record_decode(buf, n, &rec, &n) == MM_OK && rec.mono_us == 777 && rec.boot == 12);
    CHECK(mm_document_decode(rec.payload, rec.len, &field, 1, &n) == MM_OK &&
          n == 1 && field.value.as.integer == 12);
    mm_actions other_registry;
    mm_pipe_actions other_bindings;
    const mm_action *other_slots[5];
    mm_pipe_action_config push_cfg = {.pipes = &published[1], .pipe_count = 1};
    CHECK(mm_actions_init(&other_registry, other_slots, 5));
    CHECK(mm_pipe_actions_register(&other_registry, &other_bindings, &push_cfg));
    CHECK(run(mm_actions_local(&other_registry), "sync pipes") && strstr(message, "No configured"));
    CHECK(run(mm_actions_remote(&other_registry, allow_action, &session), "show pipes") &&
          strstr(message, "No matching"));
    mm_journal_close(peer);
    mm_journal_close(origin);
    CHECK(!unlink(rpath) && !unlink(opath) && !rmdir(dir));
    return 0;
}
