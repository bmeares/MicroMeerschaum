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

static mm_journal_status fetch_fail(void *ctx, const mm_pipe *pipe, size_t index, mm_row *row, bool *done)
{
    (void)ctx; (void)pipe; (void)index; (void)row; (void)done;
    return MM_JOURNAL_IO;
}

static char message[1024];
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
    char dir[] = "/tmp/mm-pipe-actions-XXXXXX", opath[128], rpath[128], opath2[128];
    CHECK(mkdtemp(dir));
    snprintf(opath, sizeof opath, "%s/origin", dir);
    snprintf(rpath, sizeof rpath, "%s/peer", dir);
    snprintf(opath2, sizeof opath2, "%s/origin2", dir);
    static const char key[] = "plugin:sample\x1f" "daily\x1f";
    mm_journal_config journal = {{11, 1, key, sizeof key - 1}, MM_JOURNAL_ORIGIN, MM_DESTINATION, 22, 22, 0};
    mm_volume_options options = {4, 1024, MM_KEEP_OLD};
    mm_journal *origin, *peer, *origin2;
    CHECK(mm_journal_open_volume(&origin, opath, &journal, &options, true) == MM_JOURNAL_OK);
    journal.mode = MM_JOURNAL_RECEIVER;
    CHECK(mm_journal_open_volume(&peer, rpath, &journal, &options, true) == MM_JOURNAL_OK);
    static const char key2[] = "plugin:flaky\x1f" "hourly\x1f" "south";
    mm_journal_config journal2 = {{11, 1, key2, sizeof key2 - 1}, MM_JOURNAL_ORIGIN, MM_DESTINATION, 22, 22, 0};
    CHECK(mm_journal_open_volume(&origin2, opath2, &journal2, &options, true) == MM_JOURNAL_OK);
    mm_pipe sensor, push_only, flaky;
    uint8_t scratch[64], scratch2[64];
    CHECK(mm_pipe_open(&sensor, "plugin:sample", "daily", NULL, mm_pipe_journal_target(origin),
                       scratch, sizeof scratch) == MM_JOURNAL_OK);
    CHECK(mm_pipe_bind_source(&sensor, "sample", fetch, NULL) == MM_JOURNAL_OK);
    static const char other[] = "driver\x1f" "daily\x1f" "north";
    mm_pipe_target target = {{11, 1, other, sizeof other - 1}, push, NULL};
    CHECK(mm_pipe_open(&push_only, "driver", "daily", "north", target, NULL, 0) == MM_JOURNAL_OK);
    CHECK(mm_pipe_open(&flaky, "plugin:flaky", "hourly", "south", mm_pipe_journal_target(origin2),
                       scratch2, sizeof scratch2) == MM_JOURNAL_OK);
    CHECK(mm_pipe_bind_source(&flaky, "flaky", fetch_fail, NULL) == MM_JOURNAL_OK);
    const mm_action_pipe published[] = {
        {&sensor, "journal:local", origin},
        {&push_only, "journal:other", NULL}
    };
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

    /* Pipe(...) formatting and singular aliases. */
    CHECK(run(local, "show pipes") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n"
                           "Pipe('driver', 'daily', 'north', instance='journal:other')\n"));
    CHECK(run(local, "show pipe") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n"
                           "Pipe('driver', 'daily', 'north', instance='journal:other')\n"));
    CHECK(run(remote, "show pipes") && !strcmp(message, "Pipe('plugin:sample', 'daily')\n"));
    CHECK(run(remote, "show pipe") && !strcmp(message, "Pipe('plugin:sample', 'daily')\n"));
    CHECK(run(local, "help show pipe") && strstr(message, "show pipes (show pipe)"));
    CHECK(run(local, "help sync pipe") && strstr(message, "sync pipes (sync pipe)"));
    CHECK(run(local, "help copy pipe") && strstr(message, "copy pipes (copy pipe)"));

    CHECK(!run(remote, "sync pipes") && mm_journal_high(origin) == 0);
    CHECK(!run(remote, "sync pipe") && mm_journal_high(origin) == 0);
    CHECK(!run(remote, "copy pipes peer:lab") && !link.deliveries);
    CHECK(!run(remote, "copy pipe -I peer:lab") && !link.deliveries);
    CHECK(!run(local, "sync pipes -m daily") && mm_journal_high(origin) == 0);

    /* Selector parity: None, [None], '', multiple values, repeated flags, _ exclusions, bounds. */
    CHECK(run(local, "show pipes -l None") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n"));
    CHECK(run(local, "show pipes -l [None]") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n"));
    CHECK(run(local, "show pipes --location-keys ''") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n"));
    CHECK(run(local, "show pipes -l _[None]") &&
          !strcmp(message, "Pipe('driver', 'daily', 'north', instance='journal:other')\n"));
    CHECK(run(local, "show pipes -l _None") &&
          !strcmp(message, "Pipe('driver', 'daily', 'north', instance='journal:other')\n"));
    CHECK(run(local, "show pipes -l _north") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n"));
    CHECK(run(local, "show pipes -l [None] north") &&
          strstr(message, "Pipe('plugin:sample', 'daily')") &&
          strstr(message, "Pipe('driver', 'daily', 'north', instance='journal:other')"));
    CHECK(run(local, "show pipes -c plugin:sample driver") &&
          strstr(message, "Pipe('plugin:sample', 'daily')") &&
          strstr(message, "Pipe('driver', 'daily', 'north', instance='journal:other')"));
    CHECK(run(local, "show pipes -c plugin:sample --connector-keys driver _driver") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n"));
    CHECK(run(local, "show pipes -c _driver") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n"));
    CHECK(run(local, "show pipes -m daily --metric-keys daily") &&
          strstr(message, "Pipe('plugin:sample', 'daily')"));
    CHECK(run(local, "show pipes --mrsm-instance journal:other") &&
          !strcmp(message, "Pipe('driver', 'daily', 'north', instance='journal:other')\n"));
    CHECK(run(local, "show pipes -i journal:local journal:other") &&
          strstr(message, "Pipe('plugin:sample', 'daily')") &&
          strstr(message, "Pipe('driver', 'daily', 'north', instance='journal:other')"));
    CHECK(run(local, "show pipes -i _journal:other") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n"));
    CHECK(run(local, "show pipes -c a b c d e f g h") && strstr(message, "No matching"));
    CHECK(!run(local, "show pipes -c a b c d e f g h i"));
    CHECK(run(local, "show pipes -c missing") && strstr(message, "No matching"));
    CHECK(!run(local, "sync pipes -c missing"));
    CHECK(!run(local, "copy pipes -c plugin:sample"));
    CHECK(!run(local, "copy pipes peer:unconfigured -c plugin:sample") && !link.deliveries);
    CHECK(!run(local, "copy pipes -I peer:unconfigured -c plugin:sample") && !link.deliveries);
    CHECK(!run(local, "copy pipes peer:lab -I peer:lab") && !link.deliveries);
    CHECK(!run(local, "copy pipes -I peer:lab peer:lab") && !link.deliveries);
    CHECK(!run(local, "copy pipes -I peer:lab --target-instance peer:lab") && !link.deliveries);
    CHECK(!run(local, "copy pipes -I") && !link.deliveries);
    CHECK(!run(local, "show pipes -I peer:lab"));
    CHECK(!run(local, "sync pipes -I peer:lab"));
    CHECK(!run(local, "sync pipes --unsupported"));
    CHECK(!run(local, "sync pipes -m"));
    CHECK(!run(local, "sync pipes -c -m"));
    CHECK(!run(local, "sync pipes extra"));
    CHECK(!run(local, "sync pipes -l north") && mm_journal_high(origin) == 0);

    /* Sync and copy with status markers, aliases, exclusions, and -I / --target-instance. */
    CHECK(run(local, "sync pipes") &&
          !strcmp(message, "+ Pipe('plugin:sample', 'daily')  1 row confirmed\n"));
    CHECK(mm_journal_high(origin) == 1 && !mm_journal_high(peer));
    session.sync = true;
    CHECK(run(remote, "sync pipe -l [None] --connector-keys plugin:sample"));
    CHECK(strstr(message, "+ Pipe('plugin:sample', 'daily')  1 row confirmed") &&
          mm_journal_high(origin) == 2);
    CHECK(run(local, "sync pipe -c _driver") && mm_journal_high(origin) == 3);

    session.copy = true;
    link.lose_receipt = true;
    CHECK(!run(remote, "copy pipes -i journal:local peer:lab") && link.deliveries == 1);
    CHECK(strstr(message, "- Pipe('plugin:sample', 'daily')  ") &&
          mm_journal_confirmed(origin) == 0 && mm_journal_high(peer) == 3);
    link.lose_receipt = false;
    CHECK(run(remote, "copy pipe -I peer:lab -i journal:local") && link.deliveries == 2);
    CHECK(strstr(message, "+ Pipe('plugin:sample', 'daily')  ") &&
          strstr(message, "confirmed through 3") && mm_journal_confirmed(origin) == 3);
    CHECK(run(remote, "copy pipes --target-instance peer:lab -c plugin:sample") && link.deliveries == 3);
    CHECK(run(remote, "copy pipes peer:lab -i journal:local") && link.deliveries == 4);
    CHECK(mm_journal_high(peer) == 3);

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

    /* Multi-pipe partial failure: one source succeeds (+), one source fails (-). */
    mm_actions multi_registry;
    mm_pipe_actions multi_bindings;
    const mm_action *multi_slots[5];
    const mm_action_pipe multi_pipes[] = {
        {&sensor, "journal:local", origin},
        {&flaky, "journal:local", origin2}
    };
    mm_pipe_action_config multi_cfg = {.pipes = multi_pipes, .pipe_count = 2};
    CHECK(mm_actions_init(&multi_registry, multi_slots, 5));
    CHECK(mm_pipe_actions_register(&multi_registry, &multi_bindings, &multi_cfg));
    CHECK(!run(mm_actions_local(&multi_registry), "sync pipes"));
    CHECK(strstr(message, "+ Pipe('plugin:sample', 'daily')  1 row confirmed\n") &&
          strstr(message, "- Pipe('plugin:flaky', 'hourly', 'south')  0 rows confirmed (sync status "));

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
    mm_journal_close(origin2);
    CHECK(!unlink(rpath) && !unlink(opath) && !unlink(opath2) && !rmdir(dir));
    return 0;
}
