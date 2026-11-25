# MicroMeerschaum

MicroMeerschaum is an open-source framework for syncing Pipes between
microcontrollers and into Meerschaum. The pre-alpha C and Rust libraries read
v0 records and segments and implement draft v1 stream identities, batches and
receipts. The POSIX reference journal can collect offline and sync opaque
records to a provisioned peer. [v1 protocol](spec/protocol-v1.md) · [v0 format](spec/wire-format.md)

The first release targets two microcontrollers syncing a Pipe without an
internet connection. Each node commits its own readings locally. A destination
returns a receipt for the contiguous sequence range it has durably accounted
for, so an interrupted transfer can resume. A node can also forward the same
records to another peer or a Meerschaum instance while preserving their
original identities and lineage. [Node-to-node Pipe delivery](docs/peer-sync.md)

The first release targets MicroPython and native ESP-IDF C/C++, including
Arduino-ESP32 sketches on ESP32-S3 through the native API. These environments
share a portable C core. A sensor driver can push a frame to a local
journal. A source plugin can supply readings when its Pipe syncs. The
application chooses when to wake the radio and send committed readings in
batches. An ESP32 application that
needs offline queries and keyed updates can select the optional `sql:local`
instance, which commits each row change and its outgoing record together.
[Device API and environment support](docs/device-api.md) · [Capture and delivery example](docs/peer-sync.md#capture-now-send-on-the-applications-schedule)

The [release design](docs/release-design.md) gives the scope, outstanding
protocol work and first implementation milestone. The
[Meerschaum design reference](docs/meerschaum-design.md) maps desktop Pipe
behavior to the device and gives reproducible SQLite examples. The
[architecture evaluation](docs/architecture-evaluation.md) covers storage
engines and hardware qualification. [Glossary](GLOSSARY.md) · [Architecture decisions](docs/adr/)

## Layout

| Path | What |
|---|---|
| `spec/` | Wire-format specification |
| `vectors/` | Shared v0 and v1 fixtures from independent Python generators |
| `core/src/` | Portable C11 codecs, caller-supplied buffers |
| `core/posix/` | Host file-backed segment journal and direct peer exchange |
| `CMakeLists.txt`, `idf_component.yml` | ESP-IDF component over the C core |
| `examples/esp-idf/codec/` | Buildable native C++ application using the current record codec |
| `examples/host/sync.c` | Run offline collection and later delivery as separate processes |
| `receiver/` | Native Rust receiver crate (not FFI); `examples/mm_dump.rs` checks journal segment files |

The durable Pipe API, MicroPython module, installable Arduino library and
Meerschaum receiver plugin are planned.

## Build and test

```bash
cmake -S core -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
cargo test --manifest-path receiver/Cargo.toml
python3 vectors/generate.py && python3 vectors/generate_v1.py && git diff --exit-code vectors/
cargo run --manifest-path receiver/Cargo.toml --example mm_dump -- journal/*.seg
```

### Try a two-node sync on a POSIX host

After the CMake build, create two private directories for this example stream.
Each invocation reopens its journal. Collection works while the peer is offline.

```bash
site=$(mktemp -d)
mkdir "$site/origin" "$site/peer"
build/mm_sync_example collect "$site/origin" 21.4
build/mm_sync_example collect "$site/origin" 21.7
build/mm_sync_example send "$site/origin" "$site/peer"
build/mm_sync_example send "$site/origin" "$site/peer"
```

Both sends report `Peer confirmed through 2`. The first transfers the backlog
and reclaims confirmed segments. A new collection in the same origin starts
at sequence 3. The example moves the batch and reply in process. A transport
adapter supplies the authenticated peer identity to the same functions.
[Host journal](docs/host-journal.md) · [Direct exchange API](core/include/micromeerschaum/peer.h)

ESP-IDF projects can pin the component by commit through the IDF Component
Manager.

```yaml
dependencies:
  micromeerschaum:
    git: https://github.com/bmeares/MicroMeerschaum.git
    version: <commit>
```

With ESP-IDF active, build the C++ example from this checkout. It encodes and
decodes one v0 record in memory. With Espressif's QEMU installed, the second
command runs the firmware. On ESP-IDF 5.5.6 the emulated ESP32-S3 logs
`Decoded record 1 (34 bytes)`.

```bash
idf.py -C examples/esp-idf/codec -D IDF_TARGET=esp32s3 build
idf.py -C examples/esp-idf/codec qemu
```

## License

Apache-2.0. See [LICENSE](LICENSE).
