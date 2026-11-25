# MicroMeerschaum

MicroMeerschaum lets an ESP32-S3 collect readings into a named Pipe while its radio sleeps. When a peer comes into range, the device can send its backlog over BLE. A relay can take custody while the final destination is offline. A Meerschaum server can also receive the original records over HTTPS and publish them as ordinary Pipe rows.

A successful local `sync()` commits a reading on the device. The sender tracks delivery separately. If a connection drops after the peer commits, the sender retries the same record identities and the peer returns a receipt for the prefix it already holds. [How peer delivery works](docs/peer-sync.md)

## Choose your starting point

| Environment | Install and first result |
| --- | --- |
| **MicroPython** on ESP32-S3 with 8 MiB flash and 2 MiB quad PSRAM | Flash `micromeerschaum-0.2.1-ESP32-S3.bin`. Install a short Python source module, sample the on-chip temperature, then reboot and read the saved observation. [Firmware and first reading](docs/micropython.md) |
| **Arduino-ESP32** | In Arduino IDE choose **Sketch → Include Library → Add .ZIP Library** and select `MicroMeerschaum-0.2.1.zip`. Open **File → Examples → MicroMeerschaum → LocalPipe** and run it on an ESP32-S3 with formatted LittleFS. The highest sequence increases on each boot. [CLI installation and peer retry sketch](docs/device-api.md#what-arduino-esp32-adds) |
| **ESP-IDF** C/C++ | Add the component below to your project. Build the storage example to append two readings and verify the saved group after reopening. [Storage and provisioning](docs/device-storage.md#run-the-storage-example) · [Named Pipe API](docs/device-api.md#one-temperature-pipe-in-two-environments) |
| **Meerschaum** 5.1.5–5.1.6 | Run `mrsm install plugins micro` with Cargo available, or install the prebuilt Linux x86-64 package. Discover a device with `mrsm show pipes -i micro:bench`, push or pull rows with ordinary Pipes, and open its action shell with `mrsm micro shell -i micro:bench`. [Host connector](docs/host-device.md) · [HTTPS receiver and upgrade](receiver/plugin/README.md) |

Download the packages or the source archive for these examples from the [v0.2.1 release](https://github.com/bmeares/MicroMeerschaum/releases/tag/v0.2.1). The [qualification results](docs/qualification.md) distinguish host integration, emulated ESP32-S3 boots and the measurements to make on physical hardware.

## Try a two-node sync on a POSIX host

From the source checkout, build the example with CMake and a C/C++ compiler. Two directories stand in for independent nodes. Each command starts a fresh process.

```sh
cmake -S core -B build -DMM_SANITIZE=OFF
cmake --build build --target mm_sync_example
site=$(mktemp -d)
mkdir "$site/origin" "$site/peer"
build/mm_sync_example collect "$site/origin" 21.4
build/mm_sync_example collect "$site/origin" 21.7
build/mm_sync_example send "$site/origin" "$site/peer"
build/mm_sync_example send "$site/origin" "$site/peer"
build/mm_sync_example collect "$site/origin" 21.9
```

Both sends print `Peer confirmed through 2`. The last collection prints `Committed locally through 3`. The example transfers the batch and reply in process. The same journal can back a named document Pipe on a device. [Example source](examples/host/sync.c) · [C++ document example](examples/host/temperature.cpp)

## Write a device Pipe

After provisioning a local `sensor` / `temperature` Pipe, a MicroPython application can submit a column batch and read the local delivery state.

```python
from micromeerschaum import Pipe

temperature = Pipe("sensor", "temperature")
ok, message = temperature.sync({"tick": [41, 42], "celsius": [21.4, 21.7]})
assert ok, message
print(temperature.progress())  # high includes the two new records
```

For a source-driven Pipe, install a module implementing `fetch(pipe, **kw)` and call `Pipe("plugin:chip_sensor", "temperature").sync()` after provisioning. For indexed reads and corrections, choose the optional `sql:local` instance at registration. [MicroPython walkthrough](docs/micropython.md) · [On-device SQLite](docs/device-sqlite.md) · [HTTPS sender](docs/https-sender.md)

Native projects can pin v0.2.1 in their component's `idf_component.yml` and include `micromeerschaum/pipe.h` or `micromeerschaum/pipe.hpp`.

```yaml
dependencies:
  micromeerschaum:
    git: https://github.com/bmeares/MicroMeerschaum.git
    version: v0.2.1
```

## Run an action

From a configured host, run `mrsm micro action show actions -i micro:bench` to see the actions that device exposes to your connection. Open `mrsm micro shell -i micro:bench` to use Tab completion and run several commands interactively. Applications can register their own actions alongside `show pipes`, `sync pipes` and `copy pipes`. [MicroPython actions and shell](docs/micropython-actions.md) · [ESP-IDF and Arduino actions](docs/native-actions.md) · [Host connection setup](docs/host-device.md)

## Build and test

```sh
cmake -S core -B build/tests -DMM_WITH_SQLITE=ON
cmake --build build/tests -j4
ctest --test-dir build/tests --output-on-failure
cargo test --manifest-path receiver/Cargo.toml --locked
cargo test --manifest-path receiver/plugin/Cargo.toml --locked
python3 vectors/generate.py && python3 vectors/generate_v1.py && git diff --exit-code vectors/
```

The C and Rust codecs share byte fixtures for v0 archives and generation-scoped v1 delivery. The release also exercises independent native and MicroPython processes, board images and HTTPS deployments. [Release qualification and reproduction](docs/qualification.md) · [Wire v0](spec/wire-format.md) · [Protocol v1](spec/protocol-v1.md) · [Document format](spec/document-v1.md) · [Design decisions](docs/adr/) · [Glossary](GLOSSARY.md)

## License

[Apache-2.0](LICENSE)
