# Collect your first reading

Use the MM_ESP32_S3 firmware and its dedicated journal partition. Each Pipe has an origin node ID, a generation and a destination node ID. The origin and generation stay the same through reboots. The destination can be another device that keeps the readings offline.

For a two-device bench run, prepare the registration on your computer. This creates a configuration module with a fresh nonzero 64-bit generation and refuses to overwrite an existing one. Give each deployed node its own nonzero node ID and keep a copy of its registration with the matching peer configuration.

```sh
python3 - <<'PY'
import secrets
generation = secrets.randbelow((1 << 64) - 1) + 1
with open("device_identity.py", "x") as f:
    print("ORIGIN = 1", "DESTINATION = 2", f"GENERATION = {generation}", sep="\n", file=f)
PY
```

Copy the registration and the two example modules to the running board. The source reads the ESP32-S3's internal die temperature. [Sensor module](../examples/micropython/chip_sensor.py) · [Application](../examples/micropython/readings.py)

```sh
mpremote fs cp device_identity.py :device_identity.py
mpremote fs cp examples/micropython/chip_sensor.py :chip_sensor.py
mpremote fs cp examples/micropython/readings.py :readings.py
mpremote repl
```

On a **new board with a blank data partition**, provision the volume and sync one observation. Passing `first_boot=True` permits formatting the dedicated data partition if mounting fails. Use it only for intentional initial provisioning.

```python
from readings import open_readings

pipe = open_readings(first_boot=True)
ok, message = pipe.sync()
print(ok, message)        # True committed 1 input rows
print(pipe.read_doc(1))   # tick and celsius measured on this board
```

On the next boot, open the existing volume and verify the saved reading before sampling again. An unavailable or mismatched store raises an error. If an origin loses its journal or sequence state, preserve any surviving files, allocate a fresh generation, provision a new volume and update the receiver's registration. Reusing the old generation for a replacement stream would reuse record identities.

```python
from readings import open_readings
pipe = open_readings()
print(pipe.progress())
print(pipe.read_doc(1))
```

## Supply data from an application

On a mounted board, open a separate Pipe for a push driver. Run with `create=True` when first provisioning this volume. Omit the flag on subsequent boots.

```python
from micromeerschaum import Pipe, Timestamp, provision
from device_identity import ORIGIN, GENERATION, DESTINATION

provision("sensor", "temperature", "/mm/PUSH.MM", ORIGIN, GENERATION, DESTINATION, create=True)
pipe = Pipe("sensor", "temperature")
ok, message = pipe.sync(df={"tick": [41, 42], "celsius": [21.4, 21.7]})
assert ok, message
```

A flat dictionary supplies one row. A list or iterable of row dictionaries, a dictionary of equal-length lists or tuples, or an object with `columns` and `itertuples(index=False, name=None)` supplies a frame. Empty frames and `{}` supply zero observations. Use `[{}]` for one empty document. Fields support null, Boolean, signed 64-bit integer, finite floating point, text and `Timestamp(unix_microseconds)`. A decoded timestamp exposes its `.us` value. Float precision follows the interpreter build.

Column lengths are checked before the first write. The writer buffers up to eight documents within the configured segment budget. `pipe.confirmed_input` is the number of input rows committed to the selected target by the latest call. A later invalid row, interrupted iterator or full store can leave an earlier prefix committed. An uncertain write requires closing and reopening the store. Inspect `uncertain_first`, `uncertain_count` and the recovered sequences before resubmitting any input. The journal also tracks missed observations in `progress()["missed"]`. Eventual sequence-bearing loss declarations travel in the delivery stream. [Storage and retention](device-storage.md)

The default journal uses 16 active 4 KiB segments with keep-old retention. Set `segments`, `segment_bytes` and `retention` explicitly when provisioning a different geometry or policy. Use one canonical volume path per Pipe and serialize its use from your application. Calling `close()` closes the registered store for every handle referring to that Pipe. Reopen with `provision(..., create=False)` and the same identity and geometry. A new `Pipe(...)` handle performs no storage or radio I/O.

## Install a source

For `plugin:chip_sensor`, the Python module is named `chip_sensor.py`. Install it on the MicroPython filesystem or add it to the firmware manifest. Implement `fetch(pipe, **kw)` to return any accepted frame. A source may also implement `register(pipe, **kw)` to return a dictionary of defaults. On its first fetch, a handle runs the registration hook and then calls `fetch`. To supply options to registration, call `pipe.register(**kw)` explicitly. The returned defaults are available in `pipe.parameters`. `pipe.sync(tick=3)` forwards the keyword to the fetch function. `pipe.sync(df=...)` skips both hooks.

## Exchange a Pipe

Provision a receiving Pipe on the destination with the same connector, metric, location, origin, generation and destination. Pass `receiver=True` and its own volume path. The calling transport establishes the sender or issuer identity before passing it to the public Pipe methods. A successful receive commits the original record bytes to the named Pipe. A lost reply leaves the last confirmed sending position unchanged.

```python
# At the origin, after collecting while offline:
frame = pipe.batch(max_records=8, cap=4096)

# At the destination, on an authenticated connection:
receipt = receiving_pipe.receive(authenticated_sender, frame)

# Back at the origin, after receiving the reply:
pipe.confirm(authenticated_issuer, receipt)
print(pipe.progress()["confirmed"])
pipe.reclaim()
```

A configured synchronous transport can also drive a copy. Register a function that sends a batch over the authenticated link and returns the receipt bytes. Set `cap` to a transfer size supported by both endpoints. Each call sends the full Pipe key and observations together.

```python
from micromeerschaum import Pipe, provision_peer

provision_peer("peer:greenhouse", DESTINATION, exchange, cap=4096)
ok, message = pipe.copy_to("peer:greenhouse")
remote = Pipe("sensor", "temperature", instance="peer:greenhouse")
ok, message = remote.sync({"tick": 43, "celsius": 21.9})
```

A direct peer Pipe stages the rows in its matching default keep-old journal and succeeds after the destination acknowledges them. If the peer is offline or a reply is lost, `remote.staged_input` reports the durable local input count and `remote.confirmed_input` reports the confirmed remote input count. Call `remote.deliver()` after reconnecting to retry those identities. `pipe.copy_to()` requires a compatible, provisioned destination. It preserves original records and tracks the receiver's receipt. A sleeping peer never prevents a local journal Pipe from accepting supplied observations within its storage budget.

## Build and test on Unix

Use the MicroPython v1.28.0 Unix standard variant, a C compiler, CMake, Python and GNU Make. Run from this repository, setting `mp` to the MicroPython source checkout. The stock Unix build takes modules from the path in `MICROPYPATH`. The ESP32 board build freezes the same Python façade.

```sh
mp=/path/to/micropython
git -C "$mp" checkout v1.28.0
make -C "$mp/ports/unix" submodules
make -C "$mp/mpy-cross" -j4
make -C "$mp/ports/unix" -j4 MICROPY_PY_FFI=0 \
    BUILD="$PWD/build/mm-unix" USER_C_MODULES="$PWD/ports/micropython"
ports/micropython/tests/run.sh "$PWD/build/mm-unix/micropython"
```

The suite commits and reopens both peer volumes, retries a lost receipt and compares original bytes. It also starts a stream in native C, reopens it in MicroPython and delivers the third reading, then repeats with the two runtimes reversed. [Executable integration tests](../ports/micropython/tests/run.sh)
