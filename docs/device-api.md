# Writing a device application

The first release targets MicroPython and native ESP-IDF C/C++, including Arduino-ESP32 sketches on the same ESP32-S3 board and storage medium as the native example. The intended workflow is the same in each environment. The application opens a named Pipe, syncs readings while offline, then sends committed readings to a peer or a hosted Meerschaum instance. A receiver can handle documents from any of them with the same mapping.

## One temperature Pipe in two environments

These examples show the proposed interface. Both applications have provisioned their local journal. The Python example uses the default node. The C++ example uses a handle to the application's configured node. The native type and function names are illustrative.

```python
from micromeerschaum import Pipe

temperature = Pipe("sensor", "temperature")
ok, message = temperature.sync({"tick": [41, 42], "celsius": [21.4, 21.7]})
```

```cpp
auto temperature = node.pipe("sensor", "temperature");
const mm::Field first[]  = {{"tick", 41}, {"celsius", 21.4}};
const mm::Field second[] = {{"tick", 42}, {"celsius", 21.7}};
const mm::Row rows[] = {first, second};
auto result = temperature.sync(rows);
```

Both calls submit the same two ordered observations to the same kind of local Pipe. Success means the observations can be recovered after restart. The writer may commit them together. A driver with its own compact binary format can submit a payload from a caller-owned buffer to the segment journal. The core assigns identity and frames it. The driver can reuse the buffer when synchronous submission returns. A driver that queues samples from an interrupt owns their storage until the writer consumes them. The application decides when to wake the radio and how much backlog to send. [Capture and delivery](peer-sync.md#capture-now-send-on-the-applications-schedule)

| Application operation | MicroPython | Native ESP-IDF |
| --- | --- | --- |
| Open a Pipe | Connector, metric, optional location and instance | Same keys and instance selection through a C handle or its C++ wrapper. Opening a provisioned handle is local. |
| Supply data | A row dictionary, list of rows or equal-length columns | Borrowed, typed fields supplied as a row or a bounded batch. The receiver sees the same field names, values and row order. |
| Fetch from a source | Install or freeze a trusted module with `fetch(pipe, **kw)` and select `plugin:<name>`. | Bind the same connector name to a compiled callback during application setup. A push-only driver can supply rows directly. |
| Check results | Unpack `success, message` and inspect confirmed progress on partial writes. | Inspect an explicit status and confirmed input progress. The API works with the default ESP-IDF exception settings. |
| Read and update local rows | On a qualified SQLite build, select `sql:local`, sync keyed updates, call `get_docs` and `get_sync_time`. | Perform the same operations with bounded native row views and the same SQLite transaction for rows and outgoing records. |
| Send or receive a Pipe | Schedule delivery to a configured peer or API instance. | Use the same delivery contract from a firmware task. Either runtime can originate, receive or forward a stream. |

## What the implementations share

The portable C library owns Pipe identity, document and batch encoding, local commit and recovery, and delivery progress. Both APIs use that library for these operations. The MicroPython module converts built-in containers and calls Python source plugins. The ESP-IDF component exposes the C interface directly and provides a thin C++ Pipe wrapper. Its firmware runs independently of a Python interpreter. Keep filesystem and radio calls in the port, and keep sensor timing, wake intervals and task orchestration in the application. [MicroPython native modules](https://docs.micropython.org/en/v1.28.0/develop/cmodules.html) · [ESP-IDF C++ support](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-guides/cplusplus.html)

Changing the firmware language should preserve a stream when the replacement firmware can read the existing store, node identity and stream state. Qualify this with three readings. In MicroPython, deliver reading 1 to a peer, confirm its receipt, then commit reading 2 with the radio off. Boot native firmware against the same store. It must recover the cursor at 1 and the original bytes of reading 2. Append reading 3, reconnect, and verify that the peer now holds all three in the same generation. Repeat in the opposite direction. Losing the identity state starts a new generation. [Stream generations](adr/0006-identify-stream-generations.md)

## Support and qualification

| Environment | Available in this checkout | Evidence needed for supported device sync |
| --- | --- | --- |
| ESP-IDF C/C++ | C codec component and a buildable C++ application for ESP32-S3. [Example](../examples/esp-idf/codec/main/main.cpp) | Build a native application using the public Pipe API. Commit and reopen a batch on the chosen medium. Exchange it with a MicroPython peer over BLE GATT in both role assignments, including a lost reply and reboot. |
| MicroPython | Release design for a native module and Python source plugins. | Build the module into a named board image. Install a sensor module, sync its first row, then run the same recovery and peer tests through the Python API. |
| Arduino-ESP32 on ESP32-S3 | A standalone sketch using a temporary library passed the emulator codec test below. | Ship the native sources as an installable library. Build an example `.ino` with Arduino CLI and pass the local durability and peer retry tests on the board and medium qualified for ESP-IDF. |

For each environment, record the board, SDK or interpreter version, medium, peak RAM, commit latency and acquisition stalls at the same batch size and durability setting. That gives a firmware author a basis for choosing document batches or a compact native payload. Espressif documents the IDF-component workflow, and Arduino specifies a separate library layout for sketch installations. [Arduino as an ESP-IDF component](https://docs.espressif.com/projects/arduino-esp32/en/latest/esp-idf_component.html) · [Arduino library specification](https://docs.arduino.cc/arduino-cli/library-specification/)

### What Arduino-ESP32 adds

On the same ESP32 board, a sketch can use the native C++ Pipe API from `setup()` and `loop()`. The additional deliverables are an Arduino-installable library assembled from the shared sources, a sketch example, and an Arduino CLI build for a pinned board package. The existing ESP-IDF component is also available to projects that add Arduino to an IDF build. Both workflows can use the same journal, batch format, peer protocol and receiver.

**Codec test.** A temporary sketchbook library put the five unchanged `core/src/*.c` files, their headers and a public `MicroMeerschaum.h` include under `src/`, alongside library metadata at its root. Arduino CLI 1.6.0 and Arduino-ESP32 3.3.6 compiled the sketch for `esp32:esp32:esp32s3`. With the board option `FlashMode=dio`, the sketch ran under Espressif's QEMU. It decoded the 62-byte `data_frame` vector, re-encoded the same header and detected a flipped payload byte. With the default flash mode, the image compiled and QEMU stopped in flash initialization before `setup()`. This exercise covers sketch packaging and the in-memory v0 codec. [Shared record vector](../vectors/records.txt)

Reuse the ESP-IDF storage adapter on a supported mounted volume. For example, in Arduino-ESP32 3.3.6, `File.flush()` returns `void`, and its VFS implementation calls `fflush` and `fsync` without surfacing either result. A journal writer needs the result before reporting a successful sync or advancing a receipt. On one board and medium, verify that a failed commit reports an error and that a successful commit survives a power cut. Then interrupt a transfer after the peer commits and retry it. Keep protocol tests in the shared suite. Use the sketch to exercise the chosen board package, storage and radio configuration. Moving to a different Arduino board family requires checking its available memory, storage guarantees and transport. [Arduino-ESP32 File interface](https://github.com/espressif/arduino-esp32/blob/3.3.6/libraries/FS/src/FS.h#L57) · [VFS flush implementation](https://github.com/espressif/arduino-esp32/blob/3.3.6/libraries/FS/src/vfs_api.cpp#L352-L359)
