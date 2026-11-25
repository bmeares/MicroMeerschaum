// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include "micromeerschaum/actions.h"

static bool write_console(const char *p, size_t n, void *ctx)
{
    FILE *out = ctx;
    return fwrite(p, 1, n, out) == n && fflush(out) == 0;
}

static bool echo(const mm_action_call *call, mm_action_output *out, void *ctx)
{
    (void)ctx;
    mm_action_write(out, call->raw_tail, call->tail_len);
    return true;
}

int main(void)
{
    static const mm_action say = {"echo", "say", "Echo the original argument text.", echo, NULL};
    static const mm_action *slots[3];
    static mm_actions actions;
    static mm_action_shell shell;
    static char reply[256];

    if (!mm_actions_init(&actions, slots, 3) || !mm_actions_register(&actions, &say) ||
        !mm_action_shell_init(&shell, mm_actions_local(&actions), MM_ACTION_QUOTED,
                              reply, sizeof reply, write_console, stdout, false)) return 1;
    int byte;
    while ((byte = getchar()) != EOF && mm_action_shell_feed(&shell, (unsigned char)byte)) {}
    return ferror(stdin) || ferror(stdout);
}
