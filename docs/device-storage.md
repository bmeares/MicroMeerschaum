# Bounded device storage

Open one mounted volume file per Pipe and delivery obligation. ESP-IDF and host applications use the same group writer, segment scanner, sequence allocator, loss records and receipt logic. A file created on one can be reopened on the other. The directory-based POSIX journal remains available for existing stores. [Journal API](../core/include/micromeerschaum/journal.h) · [Directory journal](host-journal.md)

```c
const mm_volume_options storage = {
    .segments = 16, .segment_bytes = 4096, .retention = MM_KEEP_OLD
};
mm_journal *journal;
mm_journal_status st = mm_journal_open_volume(&journal, "/store/PIPE.MM",
                                              &pipe_config, &storage, provision);
```

Set `provision` during commissioning of a fresh identity. Use `false` on routine boots. The caller supplies the origin, generation, exact Pipe key and receipt scope and gets a conflict if they differ from the stored values. Give a generation one allocator. When its sequence authority is lost, provision a new generation. A damaged or incomplete file requires recovery or explicit reprovisioning. Use one canonical pathname per image, including letter case on FAT, and serialize calls on the handle. The device port excludes a second owner of that path. [Stream generations](adr/0006-identify-stream-generations.md)

## Capacity and retention

`segments` selects 2–64 active slots. An extra slot stages an incoming group while the current groups stay readable. `segment_bytes` includes the segment header, key, record headers and payloads. Its range is 512 bytes to 1 MiB. File size is 8192 bytes plus `(segments + 1) * round_up(segment_bytes, 512)`. The default geometry uses 77,824 bytes. Readers cache at most one segment. Group encoding can require another segment-sized buffer. Smaller groups reduce acquisition memory. `max_bytes` in the Pipe config can impose a smaller runtime limit on active segment bytes. Firmware owns the filesystem mount, free space and card replacement.

* **Keep old** protects previously committed observations. On a full volume an origin checkpoints the number of rejected data samples in its existing metadata pages, returns `FULL` and assigns no sequences. `mm_journal_missed` reports the pending count. After a receipt, `mm_journal_reclaim` removes eligible segments and emits a typed missed-count record if space allows. `mm_journal_flush_loss` also attempts this directly. The metadata update publishing that record clears the counter. A second submission represents another acquisition attempt.
* **Keep new** packs the submitted group and, when required, one 56-byte framed eviction declaration into the spare slot. Choose group sizes with that space reserved. The journal replaces the minimum whole prefix of segments that makes room. The declaration covers unconfirmed sequence numbers from the confirmed position plus one through the last evicted sequence. It can subsume earlier declarations about that prefix. A checked metadata update publishes the evidence and retires the prefix together. Unconfirmed missed-count evidence stays protected. If retaining it prevents an eviction, the fallback counts the new observations as missed.
* **Receiving and custody** use finite capacity in both configurations. Ingest refuses new data when it cannot fit. Identical retries use already committed records. The origin's keep-new policy is never applied to a receiver's copy of someone else's stream.

A sender encountering an evicted prefix sends its retained declaration first so even a one-record batch can advance the receiver's cumulative receipt. `mm_journal_delivery_start` selects that declaration. `mm_journal_next` iterates physically retained sequence numbers. [Tiny-batch and retention tests](../core/tests/test_volume.c)

## What a successful commit checks

The `MMJV` v1 image uses two checksummed 2048-byte metadata pages at offsets 0 and 4096. Each names the active slots, their ranges and sizes, stream state, receipt position, pending loss count, geometry and epoch. Slots beginning at offset 8192 contain complete wire-format segments. A group write checks every byte written to its spare slot and calls `fsync`. It publishes a higher epoch in the alternate page, checks a second `fsync`, then mirrors the committed page and checks the final `fsync`. Normal commits create no new filesystem names. Receipt and counter changes only update the metadata copies.

Recovery selects a valid newest epoch, validates every selected segment and restores identity, range coverage and next sequence. It repairs a missing or older page from the validated copy. Both invalid pages, corrupted active records, overlapping runs or an unexplained origin gap stop recovery. A failed checkpoint leaves the handle uncertain until closed and reopened. Reconcile the incoming group at the returned candidate sequence. When only overflow accounting was attempted, the candidate is zero. Recheck the recovered missed count before retrying. Repeat an uncertain network ingest with the same original bytes.

The FAT VFS in ESP-IDF 5.5.6 propagates `f_sync` errors. Its SDMMC disk layer returns success for `CTRL_SYNC` and relies on the card write path. A card can have controller-specific power-loss behavior. The LittleFS 1.23.3 VFS propagates errors from file synchronization. LittleFS copy-on-write and garbage collection require headroom beyond the allocated image and can amplify writes. An I/O failure while recording loss is returned to the caller. A checked filesystem call alone cannot establish physical power-cut durability for a card or flash assembly. Physical cuts, stalls, wear and commit latency remain board qualification items. [ESP-IDF FatFs VFS](https://github.com/espressif/esp-idf/blob/v5.5.6/components/fatfs/vfs/vfs_fat.c) · [SDMMC disk operations](https://github.com/espressif/esp-idf/blob/v5.5.6/components/fatfs/diskio/diskio_sdmmc.c) · [LittleFS VFS](https://github.com/joltwallet/esp_littlefs/blob/v1.23.3/src/esp_littlefs.c)

## Run the storage example

Use ESP-IDF 5.5.6 and an ESP32-S3. The example builds both the SDMMC and LittleFS mount paths. Select one in menuconfig, configure the SD clock, command and data GPIOs to match board wiring, and provide the required SD pull-ups. For on-board flash, provision a LittleFS filesystem in the `storage` partition. Disable `CONFIG_LITTLEFS_USE_ONLY_HASH` to enable file status queries. Both mount paths preserve existing files on mount failure. Enable initial provisioning in the Storage example menu for a fresh demo stream. The example uses fixed demonstration identity values.

```sh
cd examples/esp-idf/storage
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

The app appends two records, closes and reopens the image, then checks the final record. A second boot with the same image should print `Recovered group 3..4` after the first boot printed `Recovered group 1..2`. The LittleFS and SDMMC configurations cross-build for ESP32-S3. The LittleFS path can be exercised under Espressif QEMU by merging a preformatted LittleFS image at offset 0x110000 into a fresh 4 MiB emulator flash image, then booting that file twice. Keep the same writable emulator flash between boots. The host suite also injects failed barriers during eviction, damages one manifest copy, fills custody storage and resumes delivery one record at a time. [Example](../examples/esp-idf/storage/main/storage.c) · [Host test](../core/tests/test_volume.c)
