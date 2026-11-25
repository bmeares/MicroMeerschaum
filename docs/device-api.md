# Writing a device application

The ESP32-S3 release supports MicroPython, native ESP-IDF C/C++ and Arduino-ESP32 sketches. Each environment uses the same local collection and delivery workflow. The application opens a named Pipe, syncs readings while offline, then sends committed readings to a peer or a hosted Meerschaum instance. A receiver can handle documents from any of them with the same mapping.

## One temperature Pipe in two environments

For initial MicroPython provisioning, plugin installation and reboot recovery, follow the [device walkthrough](micropython.md). Both applications use a provisioned local journal. The Python example uses the default node. The native example binds a named Pipe to the application's open journal.

```python
from micromeerschaum import Pipe

temperature = Pipe("sensor", "temperature")
ok, message = temperature.sync({"tick": [41, 42], "celsius": [21.4, 21.7]})
```

```cpp
#include "micromeerschaum/pipe.hpp"
#include "micromeerschaum/journal_pipe.h"

mm_sync_result collect(mm_journal *journal)
{
    uint8_t scratch[128];
    mm::Pipe temperature;
    auto status = temperature.open("sensor", "temperature", nullptr,
                                   mm_pipe_journal_target(journal), scratch, sizeof scratch);
    if (status != MM_JOURNAL_OK) return {status, 0, 0, 0};
    const mm_field first[] = {mm::field("tick", 41), mm::field("celsius", 21.4)};
    const mm_field second[] = {mm::field("tick", 42), mm::field("celsius", 21.7)};
    const mm_row rows[] = {mm::row(first), mm::row(second)};
    return temperature.sync(rows);
}
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

## Try the native Pipe

Build the host core and run the example twice against the same directory. Each invocation commits two new observations to the same stream. The C++ wrapper builds with exceptions disabled. [Runnable example](../examples/host/temperature.cpp)

```sh
cmake -S core -B build
cmake --build build --target mm_temperature
journal_dir=$(mktemp -d)
./build/mm_temperature "$journal_dir"
./build/mm_temperature "$journal_dir"
```

For C, form the provisioned stream key with `mm_pipe_key`, bind it with `mm_pipe_open`, and pass an array of `mm_row` to `mm_pipe_sync_rows`. Each row borrows an array of typed fields. Names and text carry their byte lengths. A row with zero fields writes an empty document. A batch with zero rows writes nothing. A driver can use `mm_pipe_sync_binary` with a provisioned binary stream and reuse its buffer on return. Provision the materializer with the matching payload profile. [C interface](../core/include/micromeerschaum/pipe.h) · [Document encoding](../spec/document-v1.md)

Document sync groups up to eight rows that fit in the caller’s scratch buffer. Size it for at least the largest expected document and select a size appropriate to the target’s group limit. Restrict the call to one row for an individual commit. Pass acquisition metadata as the optional `mono_us` and `boot` arguments to the C++ wrapper or explicitly in C. To bind a compiled source, open a `plugin:weather` connector and call `bind_source("weather", fetch, context)`. Each `sync()` starts the callback index at zero and durably writes each returned row before requesting another. The source signals completion with `done = true`. Supplied frames bypass the source.

The result’s `confirmed` counts the leading input rows committed by that call. On `MM_JOURNAL_UNCERTAIN`, reopen the same generation before reusing the handle. If `uncertain_count` is positive, compare the records beginning at `uncertain_first` with the corresponding original input, including its monotonic time, boot ID and exact payload. Account for matching rows before submitting the remaining suffix. Treat different bytes at an assigned identity as a conflict. The journal test injects a failed directory sync after publishing the second group and checks the candidate records after restart. [First-use and recovery test](../core/tests/test_pipe.cpp)

The optional SQLite target uses the same Pipe handle. It adds typed keyed rows, bounded queries and an immutable outgoing record for each committed observation. [SQLite provisioning, receipt handling and device qualification](device-sqlite.md)

## What the implementations share

The portable C library owns Pipe identity, document and batch encoding, local commit and recovery, and delivery progress. Both APIs use that library for these operations. The MicroPython module converts built-in containers and calls Python source plugins. The ESP-IDF component exposes the C interface directly and provides a thin C++ Pipe wrapper. Its firmware runs independently of a Python interpreter. Keep filesystem and radio calls in the port, and keep sensor timing, wake intervals and task orchestration in the application. [MicroPython native modules](https://docs.micropython.org/en/v1.28.0/develop/cmodules.html) · [ESP-IDF C++ support](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-guides/cplusplus.html)

Changing the firmware language should preserve a stream when the replacement firmware can read the existing store, node identity and stream state. Qualify this with three readings. In MicroPython, deliver reading 1 to a peer, confirm its receipt, then commit reading 2 with the radio off. Boot native firmware against the same store. It must recover the cursor at 1 and the original bytes of reading 2. Append reading 3, reconnect, and verify that the peer now holds all three in the same generation. Repeat in the opposite direction. Losing the identity state starts a new generation. [Stream generations](adr/0006-identify-stream-generations.md)

## Support and qualification

The native ESP-IDF example runs on a mounted LittleFS volume. The MicroPython build adds a dedicated journal partition and the high-level Pipe module. Arduino packages the C/C++ Pipe, bounded volume journal and peer exchange in an installable library. For named SDK versions, capabilities exercised on the host or emulator, and the physical-board procedure, see the [release qualification](qualification.md).

### What Arduino-ESP32 adds

Install Arduino CLI 1.6.0, the ESP32 board package and the v0.1.0 library ZIP. In the commands below, `--additional-urls` adds Espressif's board package index and the environment variable enables Arduino CLI’s local ZIP installer. Run the compile commands from the matching MicroMeerschaum release source. An IDE installation exposes the same two sketches under **File → Examples → MicroMeerschaum**. [Download the Arduino ZIP](https://github.com/bmeares/MicroMeerschaum/releases/download/v0.1.0/MicroMeerschaum-0.1.0.zip) · [Release source](https://github.com/bmeares/MicroMeerschaum/archive/refs/tags/v0.1.0.tar.gz)

```sh
esp_index=https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index --additional-urls "$esp_index"
arduino-cli core install esp32:esp32@3.3.6 --additional-urls "$esp_index"
ARDUINO_LIBRARY_ENABLE_UNSAFE_INSTALL=true arduino-cli lib install --zip-path MicroMeerschaum-0.1.0.zip
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashMode=dio ports/arduino/examples/LocalPipe
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashMode=dio ports/arduino/examples/PeerRetry
```

To create the same ZIP from source, run `python3 ports/arduino/package.py MicroMeerschaum-0.1.0.zip` before installation. Use a formatted LittleFS partition for both sketches and monitor serial output at 115200 baud. **LocalPipe** writes one temperature document per boot. On a fresh volume its highest sequence rises from 1 to 2 across two boots. **PeerRetry** writes to two independent node stores on the same board, discards the first receipt and retries. The second boot prints `Peer holds through 2 after retry`. Both sketches use fixed demonstration identities. Provision distinct node IDs and stream generations when deploying them. [LocalPipe](../ports/arduino/examples/LocalPipe/LocalPipe.ino) · [PeerRetry](../ports/arduino/examples/PeerRetry/PeerRetry.ino)

The initial Arduino probe used a temporary sketch library to decode, re-encode and corrupt-check the 62-byte v0 `data_frame` vector in QEMU. The emulated board reached `setup()` with `FlashMode=dio`. The default flash mode stopped during flash initialization. The release tests installed the library ZIP with Arduino CLI and ran both sketches against persistent emulated LittleFS. The volume writer checks each filesystem sync result through the ESP VFS. The Arduino `File.flush()` interface returns `void`. [Shared vector](../vectors/records.txt) · [Volume implementation](../core/storage/volume.c) · [Arduino-ESP32 VFS implementation](https://github.com/espressif/arduino-esp32/blob/3.3.6/libraries/FS/src/vfs_api.cpp#L352-L359)
