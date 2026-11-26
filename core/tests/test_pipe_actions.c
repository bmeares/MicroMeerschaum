// SPDX-License-Identifier: Apache-2.0
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
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

typedef struct { bool list_push, sync, copy, read; } permission;

static bool allow_action(const mm_action *a, void *ctx)
{
    (void)a;
    return ctx != NULL;
}

static bool allow_only_version(const mm_action *a, void *ctx)
{
    (void)ctx;
    return !strcmp(a->name, "show version");
}

static bool allow_pipe(const mm_action_view *v, const mm_action_pipe *p, mm_pipe_action_kind op,
                       const mm_action_destination *dest, void *ctx)
{
    (void)ctx;
    if (v->local) return true;
    const permission *session = v->user;
    if (op == MM_PIPE_SHOW) return p->pipe->fetch || session->list_push;
    if (op == MM_PIPE_SYNC) return session->sync;
    if (op == MM_PIPE_COPY) return session->copy && dest && !strcmp(dest->name, "peer:lab");
    return session->read;
}

typedef struct {
    bool is_sql;
    mm_journal *journal;
    bool fail_inspect;
    unsigned data_calls;
    size_t last_limit;
    char last_begin[64];
    char last_end[64];
} inspect_state;

static bool inspect_data(const mm_action_pipe *p, const mm_pipe_data_request *req,
                         mm_action_output *out, void *ctx)
{
    (void)ctx;
    inspect_state *st = p->user;
    if (!st) return false;
    st->data_calls++;
    st->last_limit = req->limit;
    snprintf(st->last_begin, sizeof st->last_begin, "%s", req->begin ? req->begin : "");
    snprintf(st->last_end, sizeof st->last_end, "%s", req->end ? req->end : "");
    if (!st->is_sql) {
        if (req->begin || req->end) {
            mm_action_puts(out, "  journal pipes do not support --begin or --end\n");
            return false;
        }
        uint64_t seq = 1;
        size_t shown = 0;
        while (shown < req->limit && mm_journal_next(st->journal, seq, &seq) == MM_JOURNAL_OK) {
            uint8_t buf[128];
            size_t n = 0;
            mm_record rec;
            mm_field fields[4];
            size_t fc = 0;
            if (mm_journal_read(st->journal, seq, buf, sizeof buf, &n) == MM_JOURNAL_OK &&
                mm_record_decode(buf, n, &rec, &n) == MM_OK && rec.kind == MM_KIND_DATA &&
                mm_document_decode(rec.payload, rec.len, fields, 4, &fc) == MM_OK && fc == 1) {
                char line[64];
                snprintf(line, sizeof line, "  {tick: %" PRId64 "}\n", fields[0].value.as.integer);
                mm_action_puts(out, line);
                shown++;
            }
            seq++;
        }
        if (!shown) mm_action_puts(out, "  (no rows)\n");
        return true;
    }
    if (req->limit == 0) {
        mm_action_puts(out, "  (no rows)\n");
        return true;
    }
    char line[128];
    snprintf(line, sizeof line, "  sql limit=%zu begin=%s end=%s\n", req->limit,
             req->begin ? req->begin : "-", req->end ? req->end : "-");
    mm_action_puts(out, line);
    return true;
}

static bool inspect_columns(const mm_action_pipe *p, mm_action_output *out, void *ctx)
{
    (void)ctx;
    const inspect_state *st = p->user;
    if (!st) return false;
    if (st->is_sql) {
        mm_action_puts(out, "  tick: int [axis]\n  station: string [key]\n  celsius: float\n");
    } else {
        mm_action_puts(out, "  (no fixed schema; journal pipe)\n");
    }
    return true;
}

static bool inspect_rowcount(const mm_action_pipe *p, uint64_t *count, void *ctx)
{
    (void)ctx;
    const inspect_state *st = p->user;
    if (!st || st->fail_inspect) return false;
    if (st->is_sql) {
        *count = 3;
        return true;
    }
    uint64_t seq = 1, n = 0;
    while (mm_journal_next(st->journal, seq, &seq) == MM_JOURNAL_OK) {
        n++;
        seq++;
    }
    *count = n;
    return true;
}

static bool inspect_progress(const mm_action_pipe *p, mm_pipe_progress *prog, void *ctx)
{
    (void)ctx;
    const inspect_state *st = p->user;
    if (!st || st->fail_inspect) return false;
    if (st->is_sql) {
        *prog = (mm_pipe_progress){5, 5, 2, 0};
        return true;
    }
    *prog = (mm_pipe_progress){
        mm_journal_prefix(st->journal),
        mm_journal_high(st->journal),
        mm_journal_confirmed(st->journal),
        mm_journal_missed(st->journal)
    };
    return true;
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
    permission session = {false, false, false, false};
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

    /* Opt-in inspection actions: minimal (connectors + version only), partial, and full. */
    inspect_state journal_state = {.is_sql = false, .journal = origin};
    inspect_state sql_state = {.is_sql = true, .journal = NULL};
    const mm_action_pipe inspect_list[] = {
        {&sensor, "journal:local", &journal_state},
        {&push_only, "sql:local", &sql_state}
    };
    const mm_action_destination dests[] = {
        {"peer:lab", deliver, &link},
        {"api:main", deliver, &link}
    };
    mm_pipe_action_config inspect_pipes_cfg = {inspect_list, 2, dests, 2, allow_pipe, clock_us, 12, NULL};

    /* Minimal registration (inspect == NULL) needs only 2 extra slots and rolls back atomically. */
    mm_actions min_reg;
    mm_pipe_inspect_actions min_ins;
    const mm_action *min_slots[4];
    CHECK(mm_actions_init(&min_reg, min_slots, 3));
    CHECK(!mm_pipe_inspect_register(&min_reg, &min_ins, &inspect_pipes_cfg, NULL) && min_reg.count == 2);
    CHECK(mm_actions_init(&min_reg, min_slots, 4));
    CHECK(mm_pipe_inspect_register(&min_reg, &min_ins, &inspect_pipes_cfg, NULL) && min_reg.count == 4);
    CHECK(run(mm_actions_local(&min_reg), "show version") &&
          !strcmp(message, "MicroMeerschaum " MM_VERSION_STRING " (native)\n"));
    CHECK(!run(mm_actions_local(&min_reg), "show version extra"));
    CHECK(!run(mm_actions_local(&min_reg), "show data"));
    CHECK(run(mm_actions_local(&min_reg), "show connectors") &&
          !strcmp(message, "journal:local\nsql:local\npeer:lab\napi:main\n"));
    CHECK(run(mm_actions_local(&min_reg), "show connector peer") &&
          !strcmp(message, "peer:lab\n"));
    CHECK(run(mm_actions_local(&min_reg), "show connector api") &&
          !strcmp(message, "api:main\n"));
    CHECK(run(mm_actions_local(&min_reg), "show connector sql") &&
          !strcmp(message, "sql:local\n"));
    CHECK(run(mm_actions_local(&min_reg), "show connector journal") &&
          !strcmp(message, "journal:local\n"));
    CHECK(!run(mm_actions_local(&min_reg), "show connectors bogus"));
    CHECK(!run(mm_actions_local(&min_reg), "show connectors peer extra"));

    /* Partial registration: progress + custom version only (3 extra slots). */
    mm_actions part_reg;
    mm_pipe_inspect_actions part_ins;
    const mm_action *part_slots[5];
    mm_pipe_inspect_config part_cfg = {
        .progress = inspect_progress,
        .version = "MicroMeerschaum 0.2.1 (esp32)"
    };
    CHECK(mm_actions_init(&part_reg, part_slots, 5));
    CHECK(mm_pipe_inspect_register(&part_reg, &part_ins, &inspect_pipes_cfg, &part_cfg) && part_reg.count == 5);
    CHECK(run(mm_actions_local(&part_reg), "show version") &&
          !strcmp(message, "MicroMeerschaum 0.2.1 (esp32)\n"));
    CHECK(run(mm_actions_local(&part_reg), "show progress -i sql:local") &&
          !strcmp(message, "Pipe('driver', 'daily', 'north', instance='sql:local')  "
                           "prefix=5 high=5 confirmed=2 missed=0\n"));
    CHECK(!run(mm_actions_local(&part_reg), "show data"));
    CHECK(!run(mm_actions_local(&part_reg), "show columns"));
    CHECK(!run(mm_actions_local(&part_reg), "show rowcounts"));

    /* Full inspection registration alongside standard pipe actions (2 + 3 + 6 = 11 slots). */
    mm_actions full_reg;
    mm_pipe_actions full_pipes;
    mm_pipe_inspect_actions full_ins;
    const mm_action *full_slots[11];
    mm_pipe_inspect_config bad_ver = {.version = "bad\nver"};
    mm_pipe_inspect_config full_cfg = {
        .data = inspect_data,
        .columns = inspect_columns,
        .rowcount = inspect_rowcount,
        .progress = inspect_progress,
        .version = "MicroMeerschaum 0.2.1 (host)"
    };
    CHECK(mm_actions_init(&full_reg, full_slots, 10));
    CHECK(mm_pipe_actions_register(&full_reg, &full_pipes, &inspect_pipes_cfg));
    CHECK(!mm_pipe_inspect_register(&full_reg, &full_ins, &inspect_pipes_cfg, &full_cfg) &&
          full_reg.count == 5);
    CHECK(mm_actions_init(&full_reg, full_slots, 11));
    CHECK(mm_pipe_actions_register(&full_reg, &full_pipes, &inspect_pipes_cfg));
    CHECK(!mm_pipe_inspect_register(&full_reg, &full_ins, &inspect_pipes_cfg, &bad_ver) &&
          full_reg.count == 5);
    CHECK(mm_pipe_inspect_register(&full_reg, &full_ins, &inspect_pipes_cfg, &full_cfg) &&
          full_reg.count == 11);
    CHECK(!mm_pipe_inspect_register(&full_reg, &full_ins, &inspect_pipes_cfg, &full_cfg) &&
          full_reg.count == 11);

    mm_action_view flocal = mm_actions_local(&full_reg);
    CHECK(run(flocal, "help show datum") && strstr(message, "show data (show datum)"));
    CHECK(run(flocal, "help show column") && strstr(message, "show columns (show column)"));
    CHECK(run(flocal, "help show rowcount") && strstr(message, "show rowcounts (show rowcount)"));
    CHECK(run(flocal, "help show connector") && strstr(message, "show connectors (show connector)"));
    CHECK(run(flocal, "help show progress") && strstr(message, "show progress"));
    CHECK(run(flocal, "help show version") && strstr(message, "show version"));

    /* show data / show datum: selectors, --limit, --begin, --end, bounds, rejections. */
    CHECK(run(flocal, "show data -i journal:local --limit 2") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n  {tick: 12}\n  {tick: 12}\n") &&
          journal_state.last_limit == 2);
    CHECK(run(flocal, "show datum -l [None] --limit 0") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n  (no rows)\n") &&
          journal_state.last_limit == 0);
    CHECK(!run(flocal, "show data -i journal:local --begin 1") &&
          strstr(message, "do not support --begin or --end"));
    CHECK(run(flocal, "show datum -i sql:local --limit 5 --begin -10 --end 2026-01-02T00:00:00Z") &&
          strstr(message, "Pipe('driver', 'daily', 'north', instance='sql:local')\n") &&
          strstr(message, "sql limit=5 begin=-10 end=2026-01-02T00:00:00Z") &&
          !strcmp(sql_state.last_begin, "-10") &&
          !strcmp(sql_state.last_end, "2026-01-02T00:00:00Z"));
    CHECK(run(flocal, "show data -c missing") && strstr(message, "No matching"));
    CHECK(!run(flocal, "show data --limit 129"));
    CHECK(!run(flocal, "show data --limit -1"));
    CHECK(!run(flocal, "show data --limit abc"));
    CHECK(!run(flocal, "show data --limit 1 --limit 2"));
    CHECK(!run(flocal, "show data --begin"));
    CHECK(!run(flocal, "show data --begin -c sensor"));
    CHECK(!run(flocal, "show data --begin 1 --begin 2"));
    CHECK(!run(flocal, "show data --end"));
    CHECK(!run(flocal, "show data --end 1 --end 2"));
    CHECK(!run(flocal, "show data --unknown 1"));

    /* show columns / show column, show rowcounts / show rowcount, show progress. */
    CHECK(run(flocal, "show columns") &&
          strstr(message, "Pipe('plugin:sample', 'daily')\n  (no fixed schema; journal pipe)\n") &&
          strstr(message, "Pipe('driver', 'daily', 'north', instance='sql:local')\n  tick: int [axis]\n  station: string [key]\n  celsius: float\n"));
    CHECK(run(flocal, "show column -i _journal:local") &&
          !strstr(message, "journal pipe") && strstr(message, "tick: int [axis]"));
    CHECK(run(flocal, "show rowcounts") &&
          strstr(message, "Pipe('plugin:sample', 'daily')  4\n") &&
          strstr(message, "Pipe('driver', 'daily', 'north', instance='sql:local')  3\n"));
    CHECK(run(flocal, "show rowcount -l _[None]") &&
          !strcmp(message, "Pipe('driver', 'daily', 'north', instance='sql:local')  3\n"));
    CHECK(run(flocal, "show progress -i journal:local") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')  prefix=4 high=4 confirmed=3 missed=0\n"));
    sql_state.fail_inspect = true;
    CHECK(!run(flocal, "show rowcounts -i sql:local") && strstr(message, "rowcount unavailable"));
    CHECK(!run(flocal, "show progress -i sql:local") && strstr(message, "progress unavailable"));
    sql_state.fail_inspect = false;

    /* Remote inspection authorization: action allowlist, hidden pipe non-disclosure, and read gating. */
    mm_action_view ver_only = mm_actions_remote(&full_reg, allow_only_version, NULL);
    CHECK(run(ver_only, "show version") && !strcmp(message, "MicroMeerschaum 0.2.1 (host)\n"));
    CHECK(!run(ver_only, "show data") && strstr(message, "Unknown action"));
    CHECK(!run(ver_only, "show datum") && strstr(message, "Unknown action"));
    CHECK(!run(ver_only, "show columns") && strstr(message, "Unknown action"));
    CHECK(!run(ver_only, "show rowcounts") && strstr(message, "Unknown action"));
    CHECK(!run(ver_only, "show progress") && strstr(message, "Unknown action"));
    CHECK(!run(ver_only, "show connectors") && strstr(message, "Unknown action"));

    permission rperm = {.list_push = false, .sync = false, .copy = false, .read = false};
    mm_action_view fremote = mm_actions_remote(&full_reg, allow_action, &rperm);
    /* Hidden sql:local pipe is never disclosed in show connectors or inspection queries. */
    CHECK(run(fremote, "show connectors") &&
          !strcmp(message, "journal:local\npeer:lab\napi:main\n") &&
          !strstr(message, "sql:local"));
    CHECK(run(fremote, "show connectors sql") && strstr(message, "No matching connectors"));
    CHECK(run(fremote, "show data -i sql:local") && strstr(message, "No matching Pipes"));
    unsigned before_calls = journal_state.data_calls;
    /* Exposed journal pipe is denied until read permission is granted, with zero callback calls. */
    CHECK(!run(fremote, "show data") && strstr(message, "denied") &&
          journal_state.data_calls == before_calls);
    CHECK(!run(fremote, "show columns") && strstr(message, "denied"));
    CHECK(!run(fremote, "show rowcounts") && strstr(message, "denied"));
    CHECK(!run(fremote, "show progress") && strstr(message, "denied"));

    rperm.read = true;
    CHECK(run(fremote, "show data --limit 1") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n  {tick: 12}\n") &&
          journal_state.data_calls == before_calls + 1);
    CHECK(run(fremote, "show columns") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')\n  (no fixed schema; journal pipe)\n"));
    CHECK(run(fremote, "show rowcounts") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')  4\n"));
    CHECK(run(fremote, "show progress") &&
          !strcmp(message, "Pipe('plugin:sample', 'daily')  prefix=4 high=4 confirmed=3 missed=0\n"));

    mm_journal_close(peer);
    mm_journal_close(origin);
    mm_journal_close(origin2);
    CHECK(!unlink(rpath) && !unlink(opath) && !unlink(opath2) && !rmdir(dir));
    return 0;
}
