// SPDX-License-Identifier: Apache-2.0
#include "micromeerschaum/actions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); exit(1); } } while (0)

static mm_actions registry;
static mm_action_view local, remote;
static mm_action_workspace work;
static mm_action_output out;
static char reply[512];
static unsigned calls;
static const char *expected_line, *expected_tail;
static const char *const *expected_args;
static size_t expected_argc;

static bool capture(const mm_action_call *call, mm_action_output *result, void *ctx)
{
    calls++;
    if (expected_line) {
        CHECK(call->raw_len == strlen(expected_line));
        CHECK(!memcmp(call->raw_line, expected_line, call->raw_len));
        CHECK(call->tail_len == strlen(expected_tail));
        CHECK(!memcmp(call->raw_tail, expected_tail, call->tail_len));
        CHECK(call->argc == expected_argc);
        for (size_t i = 0; i < call->argc; i++) CHECK(!strcmp(call->argv[i], expected_args[i]));
    }
    CHECK(!call->argv[call->argc]);
    mm_action_puts(result, ctx);
    return true;
}

static const mm_action general = {"node", "n", "Node information.", capture, "general"};
static const mm_action check = {"node check", "ping", "Check node.", capture, "specific"};

static bool early(const mm_action_call *call, mm_action_output *result, void *ctx)
{
    (void)call;
    (void)ctx;
    calls++;
    mm_action_puts(result, "pending\n");
    if (!mm_action_flush(result)) return false;
    mm_action_puts(result, "done");
    return true;
}

static const mm_action flush_cmd = {"flush", NULL, NULL, early, NULL};

static bool admitted(const mm_action *a, void *ctx)
{
    return a != ctx;
}

static bool dispatch(const mm_action_view *view, const char *s, mm_action_mode mode)
{
    return mm_actions_dispatch(view, s, strlen(s), mode, &work, &out);
}

static void registration(void)
{
    static const mm_action *slots[6];
    CHECK(!mm_actions_init(&registry, slots, 1));
    CHECK(!mm_actions_register(&registry, &general));
    CHECK(mm_actions_init(&registry, slots, 6));
    mm_action bad = general;
    const char *invalid[] = {NULL, "", " two", "two ", "two  words", "a\tb", "a\\b", "a\""};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        bad.name = invalid[i];
        CHECK(!mm_actions_register(&registry, &bad));
    }
    bad = general;
    bad.run = NULL;
    CHECK(!mm_actions_register(&registry, &bad));
    CHECK(mm_actions_register(&registry, &general));
    CHECK(mm_actions_register(&registry, &check));
    CHECK(mm_actions_register(&registry, &flush_cmd));
    CHECK(!mm_actions_register(&registry, &general));
    bad = (mm_action){"new", "help", NULL, capture, NULL};
    CHECK(!mm_actions_register(&registry, &bad));
    bad = (mm_action){"ping", NULL, NULL, capture, NULL};
    CHECK(!mm_actions_register(&registry, &bad));
    bad = (mm_action){"new", "n", NULL, capture, NULL};
    CHECK(!mm_actions_register(&registry, &bad));
    bad.alias = "new";
    CHECK(!mm_actions_register(&registry, &bad));
    CHECK(registry.count == 5);
    mm_actions small;
    const mm_action *pair[2];
    CHECK(mm_actions_init(&small, pair, 2));
    CHECK(!mm_actions_register(&small, &general));
    mm_action custom = {"help", "?", NULL, capture, "legacy"};
    CHECK(!mm_actions_init_with_help(&small, pair, 2, &general));
    CHECK(mm_actions_init_with_help(&small, pair, 2, &custom));
    mm_action_view other = mm_actions_local(&small);
    mm_action_output_init(&out, reply, sizeof reply);
    CHECK(dispatch(&other, "?", MM_ACTION_LITERAL) && !strcmp(reply, "legacy"));
    CHECK(dispatch(&other, "show actions", MM_ACTION_LITERAL) && strstr(reply, "help (?)"));
    local = mm_actions_local(&registry);
    remote = mm_actions_remote(&registry, admitted, (void *)&check);
    mm_action_output_init(&out, reply, sizeof reply);
}

static void tokenizer(void)
{
    static const char *const cooked[] = {"a b", "", "cd", "e f", "g\\h", "\"i"};
    expected_line = "  node\t check  'a b' \"\" c'd' e\\ f 'g\\h' \\\"i   ";
    expected_tail = "'a b' \"\" c'd' e\\ f 'g\\h' \\\"i   ";
    expected_args = cooked;
    expected_argc = sizeof cooked / sizeof *cooked;
    CHECK(dispatch(&local, expected_line, MM_ACTION_QUOTED));
    CHECK(!strcmp(reply, "specific"));
    static const char *const literal[] = {"{\"name\":", "\"a", "b\"}", "'x", "\\"};
    expected_line = "ping {\"name\": \"a b\"} 'x \\";
    expected_tail = "{\"name\": \"a b\"} 'x \\";
    expected_args = literal;
    expected_argc = sizeof literal / sizeof *literal;
    CHECK(dispatch(&local, expected_line, MM_ACTION_LITERAL));
    CHECK(!strcmp(reply, "specific"));
    expected_line = NULL;
    unsigned before = calls;
    CHECK(!dispatch(&local, "n '", MM_ACTION_QUOTED) && calls == before);
    CHECK(!dispatch(&local, "n \\", MM_ACTION_QUOTED) && calls == before);
    CHECK(!dispatch(&local, "Node", MM_ACTION_QUOTED) && calls == before);
    CHECK(!dispatch(&local, "nod", MM_ACTION_QUOTED) && calls == before);
    CHECK(dispatch(&local, "node checksum", MM_ACTION_QUOTED) && !strcmp(reply, "general"));
}

static void bounds(void)
{
    unsigned before = calls;
    const char sliced[] = {'p', 'i', 'n', 'g', '\0', 'x'};
    CHECK(!mm_actions_dispatch(&local, sliced, sizeof sliced, MM_ACTION_LITERAL, &work, &out));
    CHECK(!dispatch(&local, "ping\nnode", MM_ACTION_QUOTED));
    CHECK(!dispatch(&local, "ping\rnode", MM_ACTION_LITERAL));
    CHECK(!mm_actions_dispatch(&local, sliced, 4, (mm_action_mode)77, &work, &out));
    CHECK(calls == before);
    CHECK(mm_actions_dispatch(&local, sliced, 4, MM_ACTION_LITERAL, &work, &out));
    char line[MM_ACTION_MAX_LINE + 2u];
    memset(line, 'x', sizeof line);
    memcpy(line, "n ", 2);
    CHECK(mm_actions_dispatch(&local, line, MM_ACTION_MAX_LINE, MM_ACTION_QUOTED, &work, &out));
    CHECK(!mm_actions_dispatch(&local, line, MM_ACTION_MAX_LINE + 1u, MM_ACTION_LITERAL, &work, &out));
    line[0] = 'n';
    for (size_t i = 0; i < MM_ACTION_MAX_ARGS + 1u; i++) memcpy(line + 1u + 2u*i, " x", 2);
    CHECK(mm_actions_dispatch(&local, line, 1u + 2u*MM_ACTION_MAX_ARGS, MM_ACTION_QUOTED, &work, &out));
    before = calls;
    CHECK(!mm_actions_dispatch(&local, line, 3u + 2u*MM_ACTION_MAX_ARGS, MM_ACTION_LITERAL, &work, &out));
    CHECK(calls == before);
    const char *words[MM_ACTION_MAX_ARGS];
    for (size_t i = 0; i < MM_ACTION_MAX_ARGS; i++) words[i] = "x";
    line[3u + 2u*MM_ACTION_MAX_ARGS] = '\0';
    expected_line = line;
    expected_tail = line + 2;
    expected_args = words;
    expected_argc = MM_ACTION_MAX_ARGS;
    CHECK(dispatch(&local, line, MM_ACTION_LITERAL_PREFIX));
    expected_line = NULL;
    char tiny[3];
    mm_action_output_init(&out, tiny, sizeof tiny);
    CHECK(dispatch(&local, "ping", MM_ACTION_QUOTED));
    CHECK(out.truncated && out.len == 2 && !strcmp(tiny, "sp"));
    mm_action_output_init(&out, NULL, 0);
    CHECK(dispatch(&local, "ping", MM_ACTION_QUOTED));
    CHECK(out.truncated && !out.len);
    mm_action_output_init(&out, reply, sizeof reply);
}

static void save_name(const char *name, void *ctx)
{
    const char **slot = ctx;
    *slot = name;
}

static void visibility(void)
{
    const char *candidate = NULL;
    CHECK(mm_actions_complete(&local, " \t node  c", 10, save_name, &candidate) == 1);
    CHECK(!strcmp(candidate, "node check"));
    CHECK(mm_actions_complete(&remote, "node ", 5, NULL, NULL) == 0);
    CHECK(mm_actions_complete(&remote, "p", 1, NULL, NULL) == 0);
    CHECK(mm_actions_visit(&remote, NULL, NULL) == registry.count - 1);
    CHECK(dispatch(&local, "help ping", MM_ACTION_QUOTED));
    CHECK(strstr(reply, "node check (ping)") && strstr(reply, "Check node."));
    CHECK(dispatch(&remote, "help", MM_ACTION_QUOTED));
    CHECK(strstr(reply, "node (n)") && !strstr(reply, "ping"));
    CHECK(dispatch(&remote, "show actions", MM_ACTION_QUOTED));
    CHECK(!strstr(reply, "node check"));
    CHECK(!dispatch(&remote, "help node\tcheck", MM_ACTION_QUOTED));
    CHECK(!dispatch(&remote, "help ping", MM_ACTION_QUOTED));
    unsigned before = calls;
    CHECK(!dispatch(&remote, "node check '", MM_ACTION_QUOTED) && !strcmp(reply, "Unknown action."));
    CHECK(!dispatch(&remote, "ping", MM_ACTION_QUOTED) && !strcmp(reply, "Unknown action."));
    CHECK(calls == before);
    mm_action_view closed = mm_actions_remote(&registry, NULL, NULL);
    CHECK(!dispatch(&closed, "help", MM_ACTION_QUOTED));
    CHECK(!mm_actions_visit(&closed, NULL, NULL));
    CHECK(!mm_actions_complete(&closed, "", 0, NULL, NULL));
}

struct console { char text[2048]; size_t len; bool offline; };
static bool emit(const char *p, size_t len, void *ctx)
{
    struct console *c = ctx;
    if (c->offline) return false;
    CHECK(len < sizeof c->text - c->len);
    memcpy(c->text + c->len, p, len);
    c->len += len;
    c->text[c->len] = '\0';
    return true;
}

static void flush(void)
{
    struct console c = {0};
    out.emit = emit;
    out.user = &c;
    CHECK(dispatch(&local, "flush", MM_ACTION_QUOTED));
    CHECK(!strcmp(c.text, "pending\n") && !strcmp(reply, "done"));
    unsigned before = calls;
    c.offline = true;
    CHECK(!dispatch(&local, "flush", MM_ACTION_QUOTED));
    CHECK(out.failed && calls == before + 1);
    c.offline = false;
    CHECK(!mm_action_flush(&out) && !strcmp(c.text, "pending\n"));
}

static void feed(mm_action_shell *s, const char *text)
{
    for (; *text; text++) CHECK(mm_action_shell_feed(s, (unsigned char)*text));
}

static void shell(void)
{
    static mm_action_shell s;
    struct console c = {0};
    char buf[160];
    CHECK(mm_action_shell_init(&s, local, MM_ACTION_QUOTED, buf, sizeof buf, emit, &c, true));
    unsigned before = calls;
    feed(&s, "piq\bp\177n\t\r\n");
    CHECK(calls == before + 1 && strstr(c.text, "mm> ping ") && strstr(c.text, "specific"));
    feed(&s, "n\t\003help ping\n");
    CHECK(strstr(c.text, "Check node."));
    c.len = 0;
    CHECK(mm_action_shell_init(&s, remote, MM_ACTION_LITERAL, buf, sizeof buf, emit, &c, false));
    feed(&s, "ping\n");
    CHECK(strstr(c.text, "Unknown action."));
    before = calls;
    feed(&s, "node ");
    for (size_t i = 0; i < MM_ACTION_MAX_LINE; i++) CHECK(mm_action_shell_feed(&s, 'x'));
    feed(&s, "\b\nn\n");
    CHECK(strstr(c.text, "overlong") && strstr(c.text, "general") && calls == before + 1);
    feed(&s, "n");
    CHECK(mm_action_shell_feed(&s, 0));
    feed(&s, "\n");
    CHECK(calls == before + 1);
    feed(&s, "exit");
    CHECK(!mm_action_shell_feed(&s, '\n') && !mm_action_shell_feed(&s, 'n'));
    c.offline = true;
    CHECK(!mm_action_shell_init(&s, local, MM_ACTION_QUOTED, buf, sizeof buf, emit, &c, true));
}

int main(void)
{
    registration();
    tokenizer();
    bounds();
    visibility();
    flush();
    shell();
    return 0;
}
