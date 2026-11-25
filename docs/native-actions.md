# Native device actions

Build the C example with `cmake -S core -B build && cmake --build build --target mm_action_shell` and run `build/mm_action_shell`. Enter `help`, `help say`, and `say hello`. Enter `exit` to return to the calling program. A terminal in canonical mode buffers keystrokes until Enter. The Arduino **ActionShell** sketch feeds every received byte into the same editor and provides immediate tab completion on a serial terminal. Set the terminal to 115200 baud and disable its local echo for that sketch. [C example](../examples/host/actions.c) · [Arduino sketch](../ports/arduino/examples/ActionShell/ActionShell.ino)

## Attach an existing console

Initialize an `mm_actions` registry with a caller-owned array of descriptor pointers. The built-ins `help` and `show actions` occupy two slots. An application with an existing `help` command can supply its descriptor, alias and handler to `mm_actions_init_with_help` to preserve its replies. Register each additional `mm_action` once. Its case-sensitive name and optional alias may contain multiple words separated by single spaces. Duplicate names and aliases fail registration atomically. Keep the descriptors, strings, slots and application state alive for the lifetime of the registry. [Public API](../core/include/micromeerschaum/actions.h)

An existing line reader can call the dispatcher directly. For example, this function receives a complete line without the terminal newline and collects its reply.

```c
bool run_line(const mm_action_view *view, const char *line, size_t len,
              mm_action_workspace *work, mm_action_output *out)
{
    return mm_actions_dispatch(view, line, len, MM_ACTION_LITERAL, work, out);
}
```

Initialize the output with `mm_action_output_init(&out, reply, sizeof reply)` before the first call. Each dispatch clears the previous output. A handler receives the original line and argument tail as byte spans, and decoded arguments as NUL-terminated strings. Ordinary parsing (`MM_ACTION_QUOTED`) recognizes single and double quotes and backslash escapes outside single quotes. Literal parsing preserves punctuation and splits only on spaces and tabs. Choose it for existing JSON and line-based firmware protocols. The dispatcher matches the longest exact action name or alias before parsing arguments. One submitted line runs at most one handler. Metacharacters have no execution semantics.

The boolean result describes the action outcome. `out.len` bytes contain its response and the buffer has a trailing NUL when capacity is positive. `out.truncated` reports that the caller's capacity was insufficient. A handler can set a message with `mm_action_puts` or keep writing to its application console. For an early response, install a synchronous transport callback in `out.emit` with its context in `out.user`, then call `mm_action_flush` inside the handler. A successful flush clears the buffered bytes. A failed callback sets `out.failed` and prevents another flush on this call. An interrupted reply may follow a completed side effect. The dispatcher never retries a handler.

## Admission and limits

A trusted local prompt uses `mm_actions_local(&actions)`. For a remote caller, construct a view with `mm_actions_remote(&actions, admit, session)`. The application supplies the authenticated session and authorizes each canonical descriptor by identity or name. A null predicate denies all actions. Authorization covers aliases, direct dispatch, listing, targeted help, and completion. A denied multiword action cannot fall through to a shorter action. Apply transport framing and authentication before constructing the remote view.

The default maximum is 4224 input bytes and 32 parsed arguments after the matched name. Oversized lines, excess arguments, malformed quoting and embedded control bytes fail before invocation. Lines may contain spaces and tabs. Limits can be changed consistently across the core and its consumers at compile time. Allocate one workspace per overlapping dispatch. On small tasks, keep it in application storage. The prompt includes its own bounded input and decoding buffers and uses a caller-supplied reply buffer.

For a byte-oriented port, initialize an `mm_action_shell` with its view, parsing mode, reply buffer, write callback and echo setting. Feed received bytes to `mm_action_shell_feed`. CR, LF or CRLF submits a line, Backspace and DEL erase a byte, Tab completes or lists visible names, and Ctrl-C cancels the current line. An overlong or invalid line is discarded through its terminator. The session ends on `exit`, Ctrl-D at an empty prompt, or a failed output callback. A serial driver retains ownership of its UART and connection state.

## Publish configured Pipes

Use `mm_pipe_actions_register(&actions, &bindings, &config)` to add the three Pipe commands. Provide three free action slots and keep the bindings and configuration in application storage. The configuration supplies an explicit table of open handles and their instance names. A destination has a name, a synchronous delivery callback and its context. The callback can call `mm_send_window` on the entry's journal and report its confirmed progress. [Pipe action API](../core/include/micromeerschaum/pipe_actions.h) · [Local write and receipt-backed copy example](../core/tests/test_pipe_actions.c)

| Command | Effect |
| --- | --- |
| `show pipes -c plugin:sample` | List matching admitted Pipes, printing connector, metric, location and instance. |
| `sync pipes` | Run each configured source through `mm_pipe_sync_source`, skipping push-only handles. |
| `sync pipes -m daily -l None` | Fetch and commit matching source Pipes whose location is unset. If any selected handle is push-only, the request fails before fetching. |
| `copy pipes peer:lab -i journal:local` | Deliver retained records for the selected Pipes to the named destination. Report batches and the confirmed sequence returned by the application. |

Selectors take one exact value each. Use `-c`, `-m`, `-l`, `-i` or their long forms `--connector-keys`, `--metric-keys`, `--location-keys`, and `--instance` (`--mrsm-instance` also works). An empty location or `None` selects an unset location. Partial sync and copy errors include confirmed progress. An uncertain write includes the candidate sequence when available.

For remote sessions, the application must admit the action and the Pipe. Its Pipe predicate receives the session view, published handle, requested operation and destination for copies. `MM_PIPE_SHOW` controls discovery and selection. All visible matches must also pass the requested `MM_PIPE_SYNC` or `MM_PIPE_COPY` check before the first side effect. The default Pipe predicate allows trusted local views only. Serialize access to the handles and keep the published table stable during a request.
