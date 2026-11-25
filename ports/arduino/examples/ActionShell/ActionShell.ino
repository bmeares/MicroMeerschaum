// SPDX-License-Identifier: Apache-2.0
#include <MicroMeerschaum.h>

static mm_actions actions;
static const mm_action *slots[3];
static mm_action_shell shell;
static char reply[256];

static bool writeSerial(const char *bytes, size_t len, void *) {
    return Serial.write(reinterpret_cast<const uint8_t *>(bytes), len) == len;
}

static bool uptime(const mm_action_call *call, mm_action_output *out, void *) {
    if (call->argc) {
        mm_action_puts(out, "Usage: show uptime");
        return false;
    }
    char text[32];
    snprintf(text, sizeof text, "%lu ms", millis());
    mm_action_puts(out, text);
    return true;
}

void setup() {
    Serial.begin(115200);
    static const mm_action action = {"show uptime", "uptime", "Print uptime in ms.", uptime, nullptr};
    if (mm_actions_init(&actions, slots, 3) && mm_actions_register(&actions, &action)) {
        mm_action_shell_init(&shell, mm_actions_local(&actions), MM_ACTION_QUOTED,
                             reply, sizeof reply, writeSerial, nullptr, true);
    }
}

void loop() {
    while (shell.active && Serial.available()) {
        mm_action_shell_feed(&shell, static_cast<unsigned char>(Serial.read()));
    }
    delay(1);
}
