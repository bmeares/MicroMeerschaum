# SQLite on ESP32-S3

Use the optional `sql:local` instance for a Pipe that needs current rows while disconnected. Each committed group allocates its sequences, updates its rows, stores its original framed outgoing records and advances its high sequence in one SQLite transaction. MicroPython and native firmware open the same database format and call the same C implementation. The default segment journal continues to serve collection-only Pipes. [Native instance API](../core/include/micromeerschaum/sqlite.h) · [MicroPython example](micropython.md#query-local-rows-with-sqlite)

## Open a native Pipe

Give each Pipe one private database file on the mounted filesystem. Commission it with its origin, generation, wire key, receipt policy and column definition. Pass a null definition on routine boots to require the provisioned file and restore its saved columns, axis, dtypes and keys. A mismatched registration or delivery policy fails to open. When replacing lost sequence state, issue a fresh generation. Serialize operations per handle and finalize cursors before writing or closing.

```c
static const char key[] = "sensor\x1f" "reading\x1f";
const mm_journal_config config = {
    {1, generation, key, sizeof key - 1}, MM_JOURNAL_ORIGIN, MM_DESTINATION, 2, 2, 0
};
const mm_sqlite_column columns[] = {
    {"tick", MM_VALUE_INT, false}, {"sensor_id", MM_VALUE_TEXT, true},
    {"value", MM_VALUE_INT, false}
};
const mm_sqlite_definition definition = {columns, 3, 0, false};
mm_sqlite *db;
mm_journal_status st = mm_sqlite_open(&db, "/store/readings.db", &config,
                                      commissioning ? &definition : NULL);
if (st != MM_JOURNAL_OK) return st;
mm_pipe pipe;
uint8_t scratch[512];
st = mm_pipe_open(&pipe, "sensor", "reading", NULL, mm_sqlite_target(db), scratch, sizeof scratch);
/* Use mm_pipe_sync_rows or bind a source to a plugin: connector. */
```

The declared integer or signed UTC-microsecond axis is part of the row key. Additional key columns use typed, null-safe equality. A missing or null secondary key and literal text `"None"` represent two different keys. An update modifies the supplied columns and preserves omitted non-key columns. All supplied fields must have a declared name and a matching type or null. The axis requires a non-null value. Set `autotime` on a timestamp axis and supply `mm_sqlite_set_clock` to fill a missing axis. The writer calls the UTC clock once per commit group that needs it and includes the generated timestamp in each affected outgoing document. The MicroPython clock reads the system UTC clock and rejects an uninitialized epoch before 2000. Set the device RTC before using generated time. A supplied timestamp always takes precedence.

## Read and forward

`mm_sqlite_get_docs` orders rows by axis and then insertion order. Supply an explicit limit of 0–1024. Bounds are inclusive at `begin` and exclusive at `end`. A named filter accepts one value or a list of alternatives, including null. Filters on different columns must all match. Empty lists match zero rows. The public limits are 16 declared columns, 63 UTF-8 bytes per column name and 64 total filter values. Fields returned by `mm_sqlite_next` borrow the cursor until its next step or close. `MM_JOURNAL_NOT_FOUND` signals exhaustion. `mm_sqlite_get_sync_time` returns the newest matching axis value, or a null value for an empty selection. Both operations return a distinct error status on a read failure.

`mm_sqlite_batch` returns the unconfirmed prefix that fits in the caller's buffer. Retrying through a lost reply preserves the original bytes and sequences. A fresh correction with an old event time receives a new outgoing sequence. Use `mm_sqlite_batch_after` to specify a record-count limit or send an empty cursor request. Authenticate the issuer, decode its receipt and call `mm_sqlite_confirm`. The stored role (destination or custodian), issuer, destination, stream and allocated sequence bound must match. The receipt transaction persists the cursor and removes acknowledged outgoing records together. It preserves current logical rows. `MM_JOURNAL_UNCERTAIN` requires closing and reopening. Reconcile the candidate group with `mm_sqlite_read` before submitting more input. Sequences in this SQLite profile are bounded by the positive signed 64-bit integer range.

## Build and exercise the instance

The build pins the official SQLite 3.51.3 amalgamation by SHA-256. The host uses SQLite's Unix VFS. ESP-IDF uses SQLite's pthread mutex implementation with a dedicated single-owner VFS for a writable LittleFS mount. It checks opens, reads, writes, seeks, truncation, file status, deletion and every `fsync`. One canonical absolute path admits one open connection. The filesystem commits directory mutations through LittleFS operations. The port uses a persistent rollback journal on the same volume, FULL synchronization, a 32 KiB pager cache setting, 1 KiB default pages and memory-backed temporary tables. Reserve volume space for the database and rollback journal. [Pinned engine build](../core/sqlite/engine.cmake) · [ESP VFS](../ports/esp-idf/sqlite_vfs.c) · [SQLite VFS architecture](https://sqlite.org/vfs.html) · [SQLite journal and synchronization settings](https://sqlite.org/pragma.html)

```sh
cmake -S core -B build/sqlite -DMM_WITH_SQLITE=ON
cmake --build build/sqlite -j4
ctest --test-dir build/sqlite --output-on-failure
```

Use `FETCHCONTENT_SOURCE_DIR_MM_SQLITE3` to point an offline build at the extracted pinned archive. Linux tests fail writes with ENOSPC, fail actual file barriers, and kill a child process at successive barriers during row writes and receipt reclamation. The parent reopens each database and verifies rows, outgoing bytes, allocation and confirmation through the instance API. The native reference cases use the same input and expected answers as the stock Meerschaum 5.1.5 probe. [Recovery tests](../core/tests/test_sqlite_recovery.c) · [Reference cases](../core/tests/test_sqlite.c) · [Desktop probe](evidence/meerschaum-pipe-reference.py)

```sh
cd examples/esp-idf/sqlite
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

Use ESP-IDF 5.5.6 and joltwallet/littlefs 1.23.3. Provision a writable LittleFS image in the example's 1 MiB `storage` partition. In **SQLite Pipe example**, enable fresh database provisioning when commissioning the example identity. Enable the file-synchronization failure test for a bench build. On regular boots leave provisioning disabled. The example mounts the existing filesystem, writes a correction, reopens, compares the original record bytes and reads the newest tick.

Two ESP32-S3 boots under Espressif QEMU used the same writable 4 MiB emulated flash image with LittleFS at offset `0x110000`. On each boot the test injected an EIO at the first append barrier and recovered the original sequence position before retrying. The first recovered sequence 1 at tick 41. The second recovered sequence 2 at tick 41. Board power-cut durability, wear, acquisition stalls, latency and peak heap under deployed workload remain physical qualification measurements. [ESP32-S3 example](../examples/esp-idf/sqlite/main/sqlite_example.c)
