// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "micromeerschaum/device_session.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define GET(obj, key) cJSON_GetObjectItemCaseSensitive(obj, key)

static unsigned calls, writes, reads;
static bool open_session = true, drop_reply;
static mm_device_session s;
static char last[MM_SESSION_FRAME_MAX + 1];
static bool action(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    (void)ctx;
    calls++;
    mm_action_write(out, call->raw_tail, call->tail_len);
    return true;
}
static bool admit(const mm_action *a, void *ctx)
{
    (void)ctx;
    return strcmp(a->name, "secret") != 0;
}
static bool op(const char *name, void *ctx) { (void)name; (void)ctx; return open_session; }
static bool access_pipe(const mm_session_pipe *p, mm_session_access mode, void *ctx)
{
    (void)ctx;
    return p->id != 2 && (mode != MM_SESSION_WRITE || p->id == 1);
}
static bool write_pipe(const cJSON *rows, mm_action_output *out, void *ctx)
{
    (void)ctx;
    writes++;
    CHECK(cJSON_IsArray(rows));
    mm_action_puts(out, "committed");
    return true;
}
static bool read_pipe(const cJSON *req, cJSON *rows, mm_action_output *out, void *ctx)
{
    (void)ctx; (void)out;
    reads++;
    if (GET(req, "limit")->valueint) {
        cJSON *row = cJSON_CreateObject();
        CHECK(cJSON_AddBoolToObject(row, "sample", true));
        CHECK(cJSON_AddItemToArray(rows, row));
    }
    return true;
}
static bool emit(const char *p, size_t n, void *ctx)
{
    (void)ctx;
    CHECK(n <= MM_SESSION_FRAME_MAX && n > 7 && !memcmp(p, "@micro ", 7) && p[n - 1] == '\n');
    memcpy(last, p, n);
    last[n] = 0;
    return !drop_reply;
}
static cJSON *reply(const char *args, bool expected)
{
    last[0] = 0;
    char line[MM_SESSION_FRAME_MAX];
    int n = snprintf(line, sizeof line, "@micro {\"v\":1,\"id\":\"K42\",%s}\n", args);
    size_t len = (size_t)n;
    CHECK(n > 0 && len < sizeof line);
    CHECK(mm_device_session_feed(&s, line, 5, emit, NULL) && !last[0]);
    CHECK(mm_device_session_feed(&s, line + 5, len - 5, emit, NULL) && last[0]);
    cJSON *doc = cJSON_Parse(last + 7);
    CHECK(doc && !strcmp(GET(doc, "id")->valuestring, "K42") && !!cJSON_IsTrue(GET(doc, "ok")) == expected);
    cJSON *data = cJSON_DetachItemFromObjectCaseSensitive(doc, expected ? "data" : "error");
    cJSON_Delete(doc);
    CHECK(data);
    return data;
}
#define KEY "\"key\":[\"sensor\",\"temp\",null]"
int main(void)
{
    mm_actions registry;
    const mm_action *slots[4];
    const mm_action echo = {"say", "echo", NULL, action, NULL}, secret = {"secret", NULL, NULL, action, NULL};
    CHECK(mm_actions_init(&registry, slots, 4) && mm_actions_register(&registry, &echo) && mm_actions_register(&registry, &secret));
    mm_session_pipe pipes[] = {
        {.id=1,.connector="sensor",.metric="temp",.instance="journal:local",.read=read_pipe,.write=write_pipe},
        {.id=2,.connector="hidden",.metric="temp",.instance="journal:local",.read=read_pipe},
        {.id=3,.connector="sensor",.metric="level",.instance="journal:local",.read=read_pipe}
    };
    mm_device_session_config c = {mm_actions_remote(&registry, admit, NULL), MM_ACTION_QUOTED, pipes, 3, op, access_pipe, NULL};
    CHECK(mm_device_session_init(&s, &c));
    cJSON *data = reply("\"op\":\"actions\"", true);
    CHECK(cJSON_GetArraySize(data) == 3 && !strstr(last, "secret") && !strstr(last,"echo"));
    cJSON_Delete(data);
    cJSON_Delete(reply("\"op\":\"complete\",\"line\":\"ec\"", true));
    CHECK(strstr(last, "[]"));
    cJSON_Delete(reply("\"op\":\"action\",\"line\":\"help secret\"", false));
    cJSON_Delete(reply("\"op\":\"action\",\"line\":\"echo hello\"", true));
    CHECK(calls == 1 && strstr(last, "hello"));
    cJSON_Delete(reply("\"op\":\"catalog\",\"limit\":1", true));
    CHECK(strstr(last, "\"more\":true") && strstr(last, "temp") && !strstr(last, "level"));
    cJSON_Delete(reply("\"op\":\"catalog\",\"offset\":1", true));
    CHECK(strstr(last, "level") && !strstr(last, "hidden") && strstr(last, "\"write\":false"));
    cJSON_Delete(reply("\"op\":\"write\"," KEY ",\"rows\":[{\"t\":{\"$us\":123}}]", true));
    cJSON_Delete(reply("\"op\":\"write\"," KEY ",\"rows\":[{\"n\":9007199254740993}]", false));
    cJSON_Delete(reply("\"op\":\"write\"," KEY ",\"rows\":[{\"n\":[]}]", false));
    cJSON_Delete(reply("\"op\":\"write\"," KEY ",\"rows\":[{\"x\":1,\"x\":2}]", false));
    cJSON_Delete(reply("\"op\":\"write\"," KEY ",\"rows\":[],\"unknown\":1", false));
    CHECK(writes == 1);
    cJSON_Delete(reply("\"op\":\"read\"," KEY ",\"limit\":1", true));
    CHECK(strstr(last, "\"sample\":true"));
    cJSON_Delete(reply("\"op\":\"read\"," KEY ",\"limit\":1,\"begin\":10", false));
    cJSON_Delete(reply("\"op\":\"read\"," KEY ",\"limit\":129", false));
    CHECK(reads == 1);
    open_session = false;
    cJSON_Delete(reply("\"op\":\"action\",\"line\":\"echo nope\"", false));
    open_session = true;
    const char *request = "@micro {\"v\":1,\"id\":\"a\",\"op\":\"action\",\"line\":\"echo hi\"}\n";
    size_t n = strlen(request);
    last[0] = 0;
    CHECK(mm_device_session_line(&s, request, n - 1, emit, NULL) && !last[0]);
    const char *bad[] = {
        "@micro {\"v\":01,\"id\":\"a\",\"op\":\"action\",\"line\":\"echo bad\"}\n",
        "@micro {\"v\":1,\"id\":\"a\",\"op\":\"action\",\"line\":\"echo\\u0000bad\"}\n",
        "@micro {\"v\":1,\"id\":\"a\",\"op\":\"action\",\"line\":\"echo bad\"}{}\n"
    };
    for (size_t i = 0; i < 3; i++) CHECK(mm_device_session_feed(&s, bad[i], strlen(bad[i]), emit, NULL) && !last[0]);
    char overflow[MM_SESSION_FRAME_MAX];
    memset(overflow, 'x', sizeof overflow);
    CHECK(mm_device_session_feed(&s, overflow, sizeof overflow, emit, NULL));
    CHECK(mm_device_session_feed(&s, request, n, emit, NULL) && !last[0] && calls == 1);
    CHECK(mm_device_session_feed(&s, request, 8, emit, NULL));
    mm_device_session_reset(&s);
    CHECK(mm_device_session_feed(&s, request + 8, n - 8, emit, NULL) && !last[0]);
    drop_reply = true;
    CHECK(!mm_device_session_feed(&s, request, n, emit, NULL) && calls == 2);
    return 0;
}
