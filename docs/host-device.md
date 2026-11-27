# Use a device as a Meerschaum instance

Install the `micro` plugin on the computer running Meerschaum. Each configured `micro:<label>` connects to a device application's management session. The device chooses which existing Pipes to expose and which action names to authorize. USB and BLE use the same Pipe API. [Installation and receiver upgrades](../receiver/plugin/README.md#install)

## Connect to a serial port

Start the device's USB session and close any serial monitor so Meerschaum can open the port.

For a single USB-connected board, you can connect immediately with zero configuration:

```sh
mrsm micro shell
mrsm micro action show actions
mrsm show pipes -i micro:auto
```

You can also target a serial port directly with `-i micro:/dev/ttyACM0` (or `-i micro:COM3` on Windows). When multiple USB serial ports are attached, the plugin probes them with a read-only device-session request and selects the responding MicroMeerschaum device (or lists matching ports if more than one answers).

On Linux and macOS, USB serial works with stock `python3` using Python's standard library (`termios`/`fcntl`/`select`). Installing `pyserial` is optional on POSIX and required on Windows:

```sh
python3 -m venv ~/.venvs/micro-transport
~/.venvs/micro-transport/bin/python -m pip install pyserial
```

To assign a named instance (or customize `baudrate`, `timeout`, or `python`), add a connector under `meerschaum.connectors.micro`. The example below is a complete `MRSM_CONFIG` overlay. The connector's optional `python` selects the transport interpreter (fallback: `MRSM_PYTHON`, then `python3` on PATH). Any settings under `meerschaum.connectors.micro.default` apply to auto-detected ports as well.

```json
{
  "meerschaum": {
    "connectors": {
      "micro": {
        "bench": {
          "transport": "serial",
          "port": "/dev/ttyACM0",
          "baudrate": 115200,
          "timeout": 5,
          "python": "/home/operator/.venvs/micro-transport/bin/python"
        }
      }
    }
  }
}
```

```sh
export MRSM_CONFIG="$(cat device-connector.json)"
mrsm show pipes -i micro:bench
mrsm show pipes -i micro:bench -m temperature
```

## Connect over Bluetooth

On a Linux host with BlueZ, install `bleak` in the transport interpreter. Enroll the host using the device application's authenticated pairing flow. Confirm the passkey, bond both peers and authorize the host on the device. Configure the enrolled identity address shown by BlueZ. The management service accepts operations only on an encrypted, authenticated, bonded and authorized link.

```sh
~/.venvs/micro-transport/bin/python -m pip install bleak
```

```json
{
  "meerschaum": {
    "connectors": {
      "micro": {
        "bench": {
          "transport": "ble",
          "address": "DA:10:20:30:40:50",
          "timeout": 15,
          "python": "/home/operator/.venvs/micro-transport/bin/python"
        }
      }
    }
  }
}
```

## Read and write ordinary Pipes

Use the connector, metric and location keys printed by `show pipes`. This example assumes the device exposes a writable `sensor` / `temperature` Pipe and that the local source contains compatible rows.

```python
import meerschaum as mrsm

device = mrsm.Pipe("sensor", "temperature", instance="micro:bench")
source = mrsm.Pipe("station", "thermometer", instance="sql:local")
archive = mrsm.Pipe("station", "device_readings", instance="sql:local")

rows = source.get_docs(limit=8)
if rows:
    ok, message = device.sync(rows)
    if not ok:
        raise RuntimeError(message)

rows = device.get_docs(limit=8)
if rows:
    ok, message = archive.sync(rows)
    if not ok:
        raise RuntimeError(message)
```

`device.id` and `device.attributes["parameters"]["micro"]` describe the live exposure. A read-only Pipe reports a sync lock. The device owns registration and permissions. The reserved Meerschaum connector key `mrsm` belongs to host metadata and is excluded from discovery. Hosts that record sync metrics on their instance may report a dropped metric after a device sync.

Each write accepts up to 128 flat documents within one 4096-byte frame. The device applies its store's usual append or upsert semantics. The result describes the local write. A lost reply after sending an action or a write reports an unknown outcome. Inspect device state before explicitly repeating an action.

If a write loses its reply or the device reports an error, the host blocks further writes to that Pipe, including Meerschaum's automatic `force=True` retries. Device errors may report a partially committed group. The failure message preserves that detail and provides a recovery token. Reads and writes to other Pipes remain available. After checking the Pipe and deciding which rows still need syncing, acknowledge the specific incident. The command clears only that write guard and sends no rows.

```sh
mrsm micro reconcile RECOVERY_TOKEN -i micro:bench
```

The host shares write guards across processes running as the same user. Each guard identifies the serial device or the bonded BLE address, together with the Pipe keys. On Linux, the host uses the serial device's by-id name when available. Other serial devices use the resolved port path. Use the same transport and device address for reconciliation. With USB, configure a stable port so the address continues to identify the same device. Keep the host's state directory across restarts. It defaults to `~/.local/state/micromeerschaum/writes`, or `micromeerschaum/writes` under `XDG_STATE_HOME` or Windows `LOCALAPPDATA`. A local failure before transmission clears the guard automatically.

Journal reads return the oldest retained documents. The device SQL store supports its configured half-open axis bounds and parameter filters. For example, `device.get_docs(begin=10, end=20, params={"station":"A"}, limit=4)` reads matching rows on a suitable SQL Pipe. Use a smaller limit or narrower supported bounds if the encoded result exceeds 4096 bytes. A full read succeeds only when the host can establish that every row fits. A result that fills all 128 slots requires an explicit limit or a narrower window.

The standard CLI can pull a small complete Pipe into a new SQL target. It copies the source registration and locks along with the data.

```sh
mrsm copy pipes sql:local -i micro:bench -c sensor -m temperature --sync-data all -y
```

To push a SQL Pipe into an **empty, already-exposed writable** device Pipe with identical keys, use the CLI's data-only mode for an existing destination. Meerschaum's incremental copy requires a known watermark or an empty destination. With a populated device Pipe, use `device.sync(source.get_docs(...))` with explicit bounds as above.

```sh
mrsm copy pipes micro:bench -i sql:local -c sensor -m temperature --sync-data incremental -y
```

## Run a device action

```sh
mrsm micro shell
mrsm micro action show actions
mrsm micro action 'show pipes -m temperature' -i micro:bench
mrsm micro shell -i micro:bench
```

Omit `-i` (or pass `-i micro:auto` / `-i micro:<port>`) to auto-detect a USB-connected device. For the standard desktop shell, run `mrsm -i micro:auto` or `mrsm -i micro:bench`. Use `show pipes` to inspect the device through the instance connector and `micro action 'show actions'` to ask the device for its commands. Inside `micro shell`, enter the device commands directly. The dedicated prompt uses Meerschaum's configured shell template, result icons, bottom toolbar, persistent history, and the Reedline editor. Arrow keys recall actions across sessions, Ctrl-R searches history, and Ctrl-C clears the line. Piped input receives plain replies.

Quote a whole action line containing flags to pass them to the device. In the prompt, Tab requests the permitted completions from the running device. Enter runs the line once. `exit`, `quit` or Ctrl-D returns to the host. The prompt connects through the configured serial or BLE transport.

## Exercise the host plugin

Run from the source checkout with Meerschaum on PATH and pyserial installed in the supplied transport interpreter. The test uses separate pseudo-terminals for the device and the actual CLI prompt.

```sh
cargo build --release --locked --manifest-path receiver/plugin/Cargo.toml
python3 receiver/plugin/tests/device_instance.py \
    --library receiver/plugin/target/release/libmicro.so \
    --python ~/.venvs/micro-transport/bin/python
```

Add `--micropython /path/to/unix/micropython` to test the production device session and SQLite store through the serial adapter. Build the Unix interpreter with the project's native modules and SQLite support.
