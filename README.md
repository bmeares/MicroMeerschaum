# MicroMeerschaum

Meerschaum pipes for microcontrollers: a device journal, cursor-range sync, and
durable acknowledgements that work the same over BLE, Wi-Fi or HTTPS, with
lineage recorded at every hop.

**Status: pre-alpha.** Wire format version 0 ([spec/wire-format.md](spec/wire-format.md):
records, journal segments, sync batches) is implemented in the C core and the
Rust receiver, and will change without notice.

## Layout

| Path | What |
|---|---|
| `spec/` | Wire-format specification |
| `vectors/` | Shared test vectors, written by `generate.py` (an independent Python implementation); every implementation must pass them |
| `core/` | Portable C11 device core: no heap after init, caller-supplied buffers |
| `CMakeLists.txt`, `idf_component.yml` | ESP-IDF component over the C core |
| `receiver/` | Native Rust receiver crate (not FFI); `examples/mm_dump.rs` checks journal segment files |

Planned: a C++ ESP-IDF wrapper, a MicroPython native module over the same
core, and a Meerschaum plugin over the receiver crate.

## Build and test

```bash
cmake -S core -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure
cargo test --manifest-path receiver/Cargo.toml
python3 vectors/generate.py && git diff --exit-code vectors/
cargo run --manifest-path receiver/Cargo.toml --example mm_dump -- journal/*.seg
```

ESP-IDF projects can pin the component by commit through the IDF Component
Manager:

```yaml
dependencies:
  micromeerschaum:
    git: https://github.com/bmeares/MicroMeerschaum.git
    version: <commit>
```

## License

Apache-2.0. See [LICENSE](LICENSE).
