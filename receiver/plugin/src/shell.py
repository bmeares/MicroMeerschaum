# SPDX-License-Identifier: Apache-2.0
# Executed after the embedded transport module, attached to the CLI's terminal.
import json
import os
import sys


def shell():
    config = json.loads(os.environ.pop("MICRO_SHELL_CONNECTOR"))
    label = os.environ.pop("MICRO_SHELL_LABEL")
    try:
        import readline
    except ImportError:
        readline = None
    if readline is not None:
        matches = []

        def complete(text, index):
            if index == 0:
                try:
                    matches[:] = exchange(config, {"op": "complete", "line": text})
                except Exception:
                    matches.clear()
            return matches[index] if index < len(matches) else None

        readline.set_completer_delims("")
        readline.set_completer(complete)
        if "libedit" in (readline.__doc__ or ""):
            readline.parse_and_bind("bind ^I rl_complete")
        else:
            readline.parse_and_bind("tab: complete")
    while True:
        try:
            line = input(label + "> " if sys.stdin.isatty() else "")
        except EOFError:
            break
        except KeyboardInterrupt:
            print()
            continue
        if line.strip() in ("exit", "quit"):
            break
        if line.strip():
            try:
                print(exchange(config, {"op": "action", "line": line}), flush=True)
            except Exception as exc:
                print(str(exc), file=sys.stderr, flush=True)


shell()
