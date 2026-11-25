# Release qualification

Use these results to choose an integration path and repeat the checks on your deployment. Native and Arduino ESP32-S3 tests used Espressif QEMU. The MicroPython board image ran in Espressif esp-emu 1.2.0. Both emulators used persistent flash. The mixed-runtime BLE tests used a simulated ATT link. A separate two-board test ran both ESP32-S3 firmware images, including their NimBLE hosts, with Bumble virtual controllers. The HTTPS tests used independent Meerschaum deployments.

| Surface | Configuration and observed result | Reproduce |
| --- | --- | --- |
| Wire and local Pipe | C and Rust agreed on shared v0/v1 bytes and malformed inputs. Host C/C++ tests exercised document batches, sequence continuity, gaps, conflicting records, checked write failures, loss declarations and retention at capacity. | [Build commands](../README.md#build-and-test) · [Host journal](host-journal.md) |
| ESP-IDF storage | ESP-IDF 5.5.6 cross-built native ESP32-S3 LittleFS and SDMMC/FAT variants. On one persistent LittleFS 1.23.3 image, consecutive boots recovered groups 1–2 and 3–4. The standalone C++ codec example logged `Decoded record 1 (34 bytes)` in QEMU. | [Native storage example](device-storage.md#run-the-storage-example) · [Codec example](../examples/esp-idf/codec) |
| MicroPython | Unix v1.28.0 with the native module exercised offline source fetch, supplied rows, a lost reply and reboot. C and MicroPython each reopened the other runtime’s pending journal and resumed delivery in the same generation. On ESP32-S3 (ESP-IDF 5.5.1, 8 MiB flash, 2 MiB quad PSRAM), two boots recovered journal sequences 1 and 2, updated the same SQL row and produced two outgoing records. | [Unix suite](micropython.md#build-and-test-on-unix) · [ESP32-S3 image](micropython.md#build-the-board-firmware-from-source) |
| BLE peer and custody | Native → MicroPython → native and MicroPython → native → MicroPython each forwarded original bytes over 20-byte ATT values. Each hop lost a reply after commit, restarted and retried. The relay could forward after the origin released its copy. Two emulated ESP32-S3 boards also paired with a passkey, restored their authenticated bond after disconnect and retried a custody transfer after a lost receipt. | [Mixed-runtime driver](peer-sync.md#reproduce-mixed-runtime-qualification) · [Two-board firmware test](#repeat-the-two-board-ble-test) |
| Arduino | Arduino CLI 1.6.0 and Arduino-ESP32 3.3.6 compiled both installed sketches for ESP32-S3. Two boots of **LocalPipe** advanced its sequence from 1 to 2. Two boots of **PeerRetry** confirmed 1 then 2 after deliberately dropping the first receipt for each batch. Both used LittleFS. | [Build and run the sketches](device-api.md#what-arduino-esp32-adds) |
| Optional SQLite | SQLite 3.51.3 on Linux passed keyed updates, bounded queries, ENOSPC, failed barriers and process termination during transactions. The ESP-IDF 5.5.6 ESP32-S3 example recovered from an injected sync failure on each of two QEMU boots. MicroPython Unix exercised the same native instance. | [SQLite recovery cases](device-sqlite.md#build-and-exercise-the-instance) |
| Hosted receiver | An installed native plugin on Meerschaum 5.1.5 served TLS with SQLite and PostgreSQL 18 raw stores. Both retained original bytes through concurrent routes, a lost HTTPS response and API restart. Ordered replay restored a corrected ordinary Pipe row after an interrupted checkpoint. The ESP32-S3 MicroPython firmware also uploaded two documents over emulated Wi-Fi and verified TLS to a live receiver. It confirmed through sequence 2, and replay returned both expected ordinary rows. | [Independent deployment tests](../receiver/plugin/README.md#verify-independently) |

## Actions and host instances in v0.2.0

| Surface | Acceptance result |
| --- | --- |
| Host Pipe on ESP32-S3 | Meerschaum discovered a live Pipe, pushed two SQLite source rows over a pseudo-terminal bridged to the emulated UART, pulled the rows into a second SQLite Pipe and read the same rows after a cold boot. |
| Host shell and copy | Meerschaum 5.1.5 and 5.1.6 exercised discovery, CLI selection, bounded reads and writes, action completion and interrupted writes. The standard `copy pipes` action copied a small exposed Pipe into SQL and copied a SQL Pipe into an empty, writable device Pipe. |
| MicroPython management GATT | Two ESP32-S3 instances exercised authenticated pairing, application authorization, fragmented action replies, discovery, reads and writes. The test revoked the enrolled peer and verified denial. |
| Native shell and session | The C/C++ tests exercised line editing, bounded replies, action permissions, source and push-only Pipes and a lost delivery receipt. The ESP-IDF 5.5.6 toolchain built the native management example. Arduino-ESP32 3.3.6 compiled the installed **ActionShell** sketch. |
| Hosted upgrade | HTTPS capture began using the v0.1.0 plugin. After its removal and configuration migration, the installed `micro` plugin received the missing earlier sequence, acknowledged both and replayed ordered rows. The suite ran with SQLite and PostgreSQL 18. |

To repeat the host Pipe acceptance test, install Meerschaum 5.1.5 or 5.1.6 and pyserial in your environment, with its `mrsm` on PATH. Download the matching firmware and ELF, decompress the ELF, then run from the release source. [Release downloads](https://github.com/bmeares/MicroMeerschaum/releases/tag/v0.2.0) · [Host installation](host-device.md)

```sh
xz -dk micromeerschaum-0.2.0-ESP32-S3.elf.xz
cargo build --release --locked --manifest-path receiver/plugin/Cargo.toml
python3 ports/micropython/tests/board_instance.py \
  micromeerschaum-0.2.0-ESP32-S3.bin --elf micromeerschaum-0.2.0-ESP32-S3.elf \
  --library receiver/plugin/target/release/libmicro.so
```

For the management GATT cases, add `--management` to the following two-board command. Run it once with each mode using separate results directories.

## Repeat the two-board BLE test

Run these commands at the repository root with Python 3.11 or newer and `esp-emu` v1.2.0 on your PATH. Download the board image and its matching compressed ELF from the release into the working directory. A local firmware build also works with its own application ELF. [Firmware and ELF](https://github.com/bmeares/MicroMeerschaum/releases/tag/v0.2.0) · [Espressif emulator v1.2.0](https://github.com/espressif/esp-emulator/releases/tag/v1.2.0)

```sh
ble_test_dir=$(mktemp -d)
python3 -m venv "$ble_test_dir/venv"
"$ble_test_dir/venv/bin/python" -m pip install bumble==0.0.236
git clone --depth 1 --branch v1.2.0 https://github.com/espressif/esp-emulator.git "$ble_test_dir/esp-emulator"
"$ble_test_dir/venv/bin/python" ports/micropython/tests/board_ble_pair.py \
  micromeerschaum-0.2.0-ESP32-S3.bin \
  --elf micromeerschaum-0.2.0-ESP32-S3.elf.xz \
  --efuse-tool "$ble_test_dir/esp-emulator/tools/make-efuse.py" \
  --artifacts "$ble_test_dir/results"
```

In the default custody test, the harness gives each ESP32-S3 a separate flash image and public identity, loads the release’s Python relay example at each UART, and enrolls the peers with a fresh six-digit passkey. Both firmware instances report encrypted, authenticated, bonded links with 16-byte keys. Bumble drops the first ATT read response carrying a receipt after the relay has committed sequence 1. The origin stays at `confirmed=0`. On reconnection, replay advances the origin to `confirmed=1`. The relay still has exactly one sequence with the original record bytes. The harness prints `PASS` and keeps each board’s UART transcript and emulator log in the results directory. Physical RF, power behavior and peer timing are covered by the follow-up below. [Peer example](../examples/micropython/offline_relay.py) · [Qualification harness](../ports/micropython/tests/board_ble_pair.py)

## Physical-board follow-up

RF range, reconnect timing, SD removal, brownout survival, current draw, temperature and on-device latency remain unmeasured. Use spare boards and media for the interruption cases. Record board/module revision, flash or card model, runtime, firmware version, journal geometry, group size and radio settings with each result.

1. **Reopen after power loss.** Collect a numbered group with the radio off. Log the success response externally, cut power and reopen the same store with provisioning disabled. Compare the saved record bytes, generation, highest sequence and confirmed cursor. Repeat during a commit and during reclamation. For SD, remove and reinsert the same card during a write. Reconcile any uncertain group before submitting new input. [Recovery procedure](device-storage.md#what-a-successful-commit-checks)
2. **Retry a real radio hop.** Enroll two boards, collect while the destination sleeps, then interrupt the link after the destination accepts a batch and before the origin receives its receipt. Reboot both, retry and verify one original per sequence and a contiguous receipt. Swap ESP-IDF and MicroPython roles. With a third board, let a relay take custody, power off the origin and verify onward delivery. [Bonded BLE examples and expected cursors](peer-sync.md)
3. **Measure the workload.** Log the acquisition interval and its worst observed stall, local commit durations, available heap and number of records per commit. For each radio window, record elapsed time, application batches, bytes, before/after confirmed sequence and supply current. Measure enclosure or board temperature at the same sample rate and duty cycle. Report the distribution and number of trials with the configuration.

For a quick local timing check, open a fresh **bench** journal Pipe as `pipe` and run the following at the MicroPython REPL. Each line reports the committed input count, elapsed microseconds, free Python heap immediately after sync and highest sequence. Use a fresh 16-segment volume for the eight groups. It writes real bench observations.

```python
import gc
from time import ticks_us, ticks_diff

for group in range(8):
    frame = {"tick": list(range(8 * group, 8 * group + 8)), "value": [42] * 8}
    started = ticks_us()
    ok, message = pipe.sync(frame)
    elapsed_us = ticks_diff(ticks_us(), started)
    assert ok, message
    print(pipe.confirmed_input, elapsed_us, gc.mem_free(), pipe.progress()["high"])
```

The first and last lines should end in 8 and 64 on an empty journal. For peak native memory, also capture the SDK heap low-water mark. For current and energy per delivered observation, record the radio window with an external supply monitor and divide its integrated energy by the number of newly acknowledged test observations.
