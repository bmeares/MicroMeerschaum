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

Initialize the output with `mm_action_output_init(&out, reply, sizeof reply)` before the first call. Each dispatch clears the previous output. A handler receives the original line and argument tail as byte spans, and decoded arguments as NUL-terminated strings. Ordinary parsing (`MM_ACTION_QUOTED`) recognizes single and double quotes and backslash escapes outside single quotes. Literal parsing preserves punctuation and splits only on spaces and tabs. Choose it for existing JSON and line-based firmware protocols. For a legacy callback that intentionally inspects only its first few arguments, `MM_ACTION_LITERAL_PREFIX` exposes up to `MM_ACTION_MAX_ARGS` pointers and preserves the complete raw tail even when it contains more words. The dispatcher matches the longest exact action name or alias before parsing arguments. One submitted line runs at most one handler. Metacharacters have no execution semantics.

The boolean result describes the action outcome. `out.len` bytes contain its response and the buffer has a trailing NUL when capacity is positive. `out.truncated` reports that the caller's capacity was insufficient. A handler can set a message with `mm_action_puts` or keep writing to its application console. For an early response, install a synchronous transport callback in `out.emit` with its context in `out.user`, then call `mm_action_flush` inside the handler. A successful flush clears the buffered bytes. A failed callback sets `out.failed` and prevents another flush on this call. An interrupted reply may follow a completed side effect. The dispatcher never retries a handler.

## Admission and limits

A trusted local prompt uses `mm_actions_local(&actions)`. For a remote caller, construct a view with `mm_actions_remote(&actions, admit, session)`. The application supplies the authenticated session and authorizes each canonical descriptor by identity or name. A null predicate denies all actions. Authorization covers aliases, direct dispatch, listing, targeted help, and completion. A denied multiword action cannot fall through to a shorter action. Apply transport framing and authentication before constructing the remote view.

The default maximum is 4224 input bytes and 32 parsed arguments after the matched name. Oversized lines, excess arguments, malformed quoting and embedded control bytes fail before invocation. Lines may contain spaces and tabs. Limits can be changed consistently across the core and its consumers at compile time. Allocate one workspace per overlapping dispatch. On small tasks, keep it in application storage. The prompt includes its own bounded input and decoding buffers and uses a caller-supplied reply buffer.

For a byte-oriented port, initialize an `mm_action_shell` with its view, parsing mode, reply buffer, write callback and echo setting. Feed received bytes to `mm_action_shell_feed`. CR, LF or CRLF submits a line, Backspace and DEL erase a byte, Tab completes or lists visible names, and Ctrl-C cancels the current line. An overlong or invalid line is discarded through its terminator. The session displays `[ journal:local ] >` and ends on `exit`, `quit`, Ctrl-D at an empty prompt, or a failed output callback. A serial driver retains ownership of its UART and connection state.

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

## Serve a management session

The ESP-IDF component enables the correlated JSON protocol with `CONFIG_MICROMEERSCHAUM_DEVICE_SESSION=y`. Give each connection its own `mm_device_session` and call `mm_device_session_init` with a remote action view, an operation predicate and a stable table of published `mm_session_pipe` registrations. The application grants discovery, read and write independently. Both the operation and Pipe predicates must allow access. A hidden Pipe is unavailable by key. Each registration has a positive ID and a unique connector, metric and location triple. The write callback receives one fully validated array of documents. It returns the confirmed local outcome and any partial progress in the supplied message buffer. A read callback appends bounded rows using the SDK's cJSON API. Advertise supported date bounds and parameter filters with the registration's `filters` bits. [Session API](../core/include/micromeerschaum/device_session.h)

Feed received fragments to `mm_device_session_feed`, or pass a complete prefixed line including LF to `mm_device_session_line`. Reset the reader on disconnect. Requests and replies are each limited to 4096 bytes including LF. An interrupted request can have executed. A caller losing its response must reconcile the result. In this cJSON adapter, decimal integer literals and typed timestamps are supported through ±9007199254740991. Larger integers must return an error to preserve precision.

The sample firmware accepts management frames and human commands on the same USB-to-UART console. Run `idf.py set-target esp32s3` and `idf.py build` in the example directory. For the built-in USB Serial/JTAG port, enable the example’s USB Serial/JTAG option in menuconfig. Its read-only clock Pipe is visible in the catalog. Send the following two lines with LF termination. [ESP-IDF example](../examples/esp-idf/session/main/session.c)

```text
@micro {"v":1,"id":"one","op":"catalog"}
@micro {"v":1,"id":"two","op":"read","key":["sensor","uptime",null],"limit":1}
```

For BLE, enable `CONFIG_MICROMEERSCHAUM_SESSION_GATT` and provide the enrolled peer's identity address and type in the example configuration. The example displays a fresh pairing passkey on the wired console. When attaching an existing NimBLE host, call `mm_session_gatt_register` during service registration and forward GAP subscription, security-change and disconnect events to `mm_session_gatt_event`. The enrollment callback receives the resolved identity in NimBLE's little-endian byte order. It must be safe to call from both the NimBLE host and the management worker. The client subscribes to TX notifications on an encrypted, authenticated and bonded link. The worker runs actions and storage operations off the NimBLE host stack. Each reply fragment runs through the host event queue to check the current enrollment, subscription and connection generation before transmission. The application owns advertising, pairing, storage serialization and the lifecycle of its NimBLE host. [Management service API](../core/include/micromeerschaum/session_gatt.h)

### Reproduce the native transport test

The native qualification harness boots the example's ESP32-S3 image under Espressif esp-emu v1.2.0. It uses Bumble 0.0.236 for a loopback central and two virtual controllers. Configure the example with the test central's public identity `24:0a:c4:02:00:02`, address type `0`, and the default USB-to-UART bridge. Build with ESP-IDF v5.5.6. From the example directory, assemble the image and run the harness with its matching ELF.

```sh
(cd build && python -m esptool --chip esp32s3 merge_bin -o ../firmware.bin @flash_args)
python qualify.py firmware.bin --elf build/mm_session_example.elf \
  --emulator /path/to/esp-emu --efuse-tool /path/to/esp-emulator/tools/make-efuse.py
```

The test executes a human command and a framed request on the same UART. It checks denial before pairing, enters the freshly displayed passkey, reads the published clock through fragmented notifications, and checks write denial. It leaves a partial command on the first connection, restores the bond and verifies that only a fresh request completes. The harness uses fresh scratch flash files for every run. [Native transport harness](../examples/esp-idf/session/qualify.py)

To run the host session tests against the same cJSON implementation, add `-DMM_CJSON_DIR="$IDF_PATH/components/json/cJSON"` when configuring the host core, then run CTest.
