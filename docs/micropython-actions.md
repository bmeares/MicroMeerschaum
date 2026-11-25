# Device actions on MicroPython

Open a Pipe using the application's normal boot configuration. At the Python REPL, expose that live handle and enter the action prompt. Each prompt line invokes one action. Tab completes an action name, `?` lists names, and a prefix followed by `?` lists matches. Enter `exit`, Ctrl-D or Ctrl-C to return to Python. [Provision a MicroPython Pipe](micropython.md)

```python
from micromeerschaum import dispatch, expose, make_action, shell
from readings import open_readings

pipe = open_readings()
expose(pipe, read=True)

@make_action("device label")
def label(action):
    if action:
        return False, "device label takes no arguments"
    return True, "room monitor"

print(dispatch("device label"))
shell()
```

```text
micro> show actions
copy pipes
device label
help
show actions
show pipes
sync pipes
micro> device label
room monitor
micro> sync pipes -c plugin:chip_sensor
plugin:chip_sensor temperature None journal:local  committed 1 input rows
micro> exit
>>>
```

The decorator returns the original function. A handler receives its remaining words as a list and returns a Boolean success flag and a message. The dispatcher chooses the longest registered name. Single and double quotes group arguments, and backslash escapes the next character outside single quotes. Punctuation in arguments passes through as data. Embedded newlines reject the whole invocation. The local dispatcher accepts up to 4224 UTF-8 bytes and 32 argument words.

| Action | Behavior |
| --- | --- |
| `help`, `show actions` | List admitted actions. An optional prefix narrows the list. |
| `show pipes` | Show the connector, metric, location and instance of each selected exposure. |
| `sync pipes` | Call each selected source Pipe's `sync()`. With no selectors it skips push-only Pipes. Explicitly selecting a push-only Pipe returns a message explaining that it requires rows. |
| `copy pipes peer:archive` | Call `copy_to("peer:archive")` for each selected Pipe and report its result. The destination must already be provisioned. An `api:name` destination also works. |

Each action on Pipes takes one value per selector. Use `-c` / `--connector-keys`, `-m` / `--metric-keys`, `-l` / `--location-keys` or `-i` / `--mrsm-instance` / `--instance`. For example, `copy pipes peer:archive -c plugin:chip_sensor -l None -i journal:local` selects that source's unlocated local journal. A multi-Pipe operation reports a result per attempted Pipe. Successful operations on earlier Pipes persist if a later Pipe fails. [Provisioning peer and API destinations](micropython.md#exchange-a-pipe)

## Application registries and access

`make_action`, `expose`, `dispatch`, `complete` and `shell` in `micromeerschaum` use the module's `default_actions` registry. Construct an `ActionRegistry()` for a separate application or for a different host's view of the device. Call the same methods on that object. Closing a Pipe through any handle removes the live exposure from subsequent calls, including after reopening its store. Publish the reopened Pipe explicitly. `unexpose(pipe)` withdraws access immediately. Boot code rebuilds the view by exposing the already-open Pipes it chooses to publish. [Complete application example](../examples/micropython/device_actions.py)

`expose(pipe)` makes a Pipe discoverable. Set `read=True` to allow remote row reads, and `write=True` to allow incoming rows on a provisioned local store. Each published connector, metric and location triple identifies one Pipe in this view. The device assigns each exposure a stable positive ID for its lifetime. A session further limits its available operations and canonical action names. Remote help and completion show the admitted names. For a remotely admitted `sync pipes`, all selected source Pipes require write permission. For a remote `copy pipes`, the selected Pipes require read permission.

## USB serial

Give the trusted host a dedicated USB serial stream. Start the service in device boot code, or enter the following locally on the board before connecting the host. The device processes framed requests on that stream until it closes or the local application stops it. The board's console defaults to 115200 baud. To use a separate USB-to-UART bridge, pass the configured binary UART stream as both `reader` and `writer`.

```python
from micromeerschaum import default_actions
from mm_session import DeviceSession, serve_usb

session = DeviceSession(default_actions,
                        operations=("catalog", "read", "actions", "complete", "action"),
                        allowed_actions=("help", "show actions", "show pipes", "device label"))
serve_usb(session)
```

To accept writes, publish the Pipe with `write=True` and include `"write"` in the session operations. A write sends the complete array to the existing `pipe.sync(rows)`. Its message includes the confirmed prefix when a later group fails. An interrupted mutation may already have run. Inspect the device state before issuing another mutation. Each transfer uses a single request and reply. [Device session protocol](../spec/device-session.md)

Reads require an explicit limit from 0 through 128 and must fit in one 4096-byte reply. Remote journal reads return the oldest retained documents in sequence order, skipping loss records and reclaimed ranges. A journal supports the limit selector. The optional SQLite instance returns current rows and also supports its half-open `begin` / `end` interval and `params` filters. Its catalog entry lists those filters. A result that exceeds the response budget produces an error so the caller can lower its requested limit. Timestamps travel as objects with one `$us` key containing signed Unix microseconds. [SQLite query semantics](device-sqlite.md#read-and-forward)

## Bluetooth management

The board's management service uses an RX characteristic for writes with response and a TX characteristic for notifications. The custody service uses its own UUIDs and receipts. aioble installs a complete GATT table for each service window. Finish and close an active custody window before constructing the management peripheral, and close the management window before opening custody again. Start the management advertising window on a board with a previously commissioned peer. Supply the fixed BLE identity that the operator enrolled with authenticated pairing. A remote request requires a connected, encrypted, authenticated, bonded link from one of the listed devices. Supply a registry and permissions for the operator using that link. [Passkey enrollment example](../examples/micropython/offline_relay.py)

```python
import aioble, asyncio
from mm_action_ble import ManagementPeripheral

async def management_window(session, expected_peer):
    aioble.config(addr_mode=0, bond=True, le_secure=True, mitm=True, io=3)
    service = ManagementPeripheral(session, (expected_peer,))
    try:
        await service.serve_once()
    finally:
        service.close()

# Set this to the enrolled host's fixed identity address and type.
peer = aioble.Device(0, "24:00:00:00:00:01")
asyncio.run(management_window(session, peer))
```

The two-board emulator test uses actual ESP32-S3 firmware, NimBLE, aioble and two virtual BLE controllers. The harness checks that an unbonded write leaves the journal empty, completes passkey enrollment, reconnects, reads the catalog, writes and reads a row, exercises admitted and denied actions, rejects an unknown target and a registration request, and denies the enrolled device after removal from the allowlist. Run the firmware-pair harness using its usual matching image, ELF, emulator and eFuse tool arguments with the additional `--management` flag. The Unix suite exercises the prompt, the framed serial service and both storage adapters with `ports/micropython/tests/run.sh /path/to/micropython --sqlite`. [Board test harness](../ports/micropython/tests/board_ble_pair.py) · [Unix test runner](../ports/micropython/tests/run.sh)
