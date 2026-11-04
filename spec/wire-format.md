# MicroMeerschaum wire format

Status: **draft, version 0.** Nothing here is stable. Version 1 is frozen when
the first deployment survives its site soak.

## Model

- **Device pipe.** A journal on a device, keyed like a Meerschaum pipe
  (connector, metric, location). Its axis is a monotonic sequence number.
- **Sync.** A sender transfers a cursor range of one device pipe. The receiver
  answers with the sequence through which that pipe is durable. Every hop uses
  the same exchange: BLE peer, Wi-Fi gateway or HTTPS API.
- **Lineage.** Each batch carries its origin, its sequence range and a hop list
  (node, receive time, transport).
- **Materialization.** A receiver turns device pipes into ordinary Meerschaum
  pipes.

## Conventions

- Multi-byte integers are little-endian. Offsets and sizes are in bytes.
- Every structure starts with a magic and a version byte. A decoder rejects
  versions it does not implement.
- Reserved bytes are written as zero and rejected when nonzero.
- A **device pipe key** is UTF-8 `connector` 0x1F `metric` 0x1F `location`: 1
  to 200 bytes, exactly two 0x1F separators, connector and metric non-empty,
  location possibly empty.
- **seq** numbers the records of one device pipe: it starts at 1, rises by 1 per
  record and is never reused. 0 is never a valid seq.

## Record

A journal entry and a batch entry: a 32-byte header, then the payload.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | magic `4D 52` (`MR`) |
| 2 | 1 | version: 0 |
| 3 | 1 | kind: 0 data, 1 loss |
| 4 | 4 | payload length L (u32, at most 65536) |
| 8 | 8 | seq (u64) |
| 16 | 8 | mono_us (u64): microseconds since the boot that wrote it |
| 24 | 4 | boot (u32): the device's boot counter when written |
| 28 | 4 | CRC over bytes 0..28, then the L payload bytes |
| 32 | L | payload |

A data payload is one document of the device pipe, such as a raw sensor frame.
A loss payload is UTF-8 JSON describing data the device knows it lost, such as
`{"uart_overflow":1}`.

## Segment

One journal file of one device pipe: a header, then contiguous records.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `MMSG` |
| 4 | 1 | version: 0 |
| 5 | 3 | reserved |
| 8 | 8 | first_seq (u64) |
| 16 | 4 | boot at creation (u32) |
| 20 | 4 | key length K (u32, 1 to 200) |
| 24 | K | device pipe key |
| 24+K | 4 | CRC over bytes 0..24+K |

**Scan.** Starting after the header, decode records while each decodes and its
seq is the previous seq + 1 (the first record's is first_seq). The first
failure is the write frontier: unwritten space may be 0x00 or 0xFF, and a torn
last record is simply the frontier. A scan reports the record count, first_seq,
the last seq (first_seq − 1 when there are no records), the frontier offset,
and the stop reason: `end` when the data ends exactly after a record,
`seq_gap`, or the failing record's decode status.

## Batch

A cursor range of one device pipe in transit: a header, then N records with
consecutive seq. N may be 0. The batch is exactly the header and the N records.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `MMBT` |
| 4 | 1 | version: 0 |
| 5 | 1 | hop count H (0 to 8) |
| 6 | 2 | reserved |
| 8 | 8 | origin device id (u64; Walter uses its 48-bit MAC) |
| 16 | 4 | key length K (u32, 1 to 200) |
| 20 | K | device pipe key |
| 20+K | 24·H | hops, oldest first |
| 20+K+24·H | 4 | record count N (u32) |
| 24+K+24·H | 4 | CRC over the header bytes before it |

Each hop:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | node id (u64) |
| 8 | 8 | receive time (i64, Unix microseconds) |
| 16 | 1 | transport: 0 local, 1 BLE, 2 Wi-Fi, 3 LTE, 4 USB |
| 17 | 7 | reserved |

**Ack.** A receiver answers an HTTPS batch with JSON
`{"pipe": "<key>", "origin": <u64>, "durable_through": <seq>}`. The key keeps
its 0x1F separators, JSON-escaped as `\u001f`. A binary ack for BLE comes later.

## Decoding

A decoder reports the first failing check, in this order. A missing byte is
`truncated`.

| Structure | Checks |
|---|---|
| Record | 32 bytes present; magic (`bad_magic`); version (`bad_version`); kind (`bad_kind`); L ≤ 65536 (`bad_length`); 32+L bytes present; CRC (`bad_crc`); seq ≠ 0 (`bad_seq`) |
| Segment header | 24 bytes present; magic; version; reserved (`bad_field`); 1 ≤ K ≤ 200 (`bad_key`); 28+K bytes present; CRC; first_seq ≠ 0 (`bad_seq`); key (`bad_key`) |
| Batch | 20 bytes present; magic; version; H ≤ 8 (`bad_field`); reserved (`bad_field`); 1 ≤ K ≤ 200 (`bad_key`); the whole header present; CRC; key (`bad_key`); each hop's transport, then its reserved bytes (`bad_field`); each record (record checks, then seq = previous + 1, else `seq_gap`); no bytes after the last record (`trailing`) |

Status names, shared by the implementations and the vectors: `ok`, `end`,
`truncated`, `bad_magic`, `bad_version`, `bad_kind`, `bad_length`, `bad_seq`,
`seq_gap`, `bad_key`, `bad_field`, `bad_crc`, `trailing`.

## Integrity

Checksums are CRC-32/ISO-HDLC: polynomial `0x04C11DB7` (reflected
`0xEDB88320`), initial value `0xFFFFFFFF`, reflected input and output, final XOR
`0xFFFFFFFF`. The check value of the ASCII string `123456789` is `0xCBF43926`.
The C core and the Rust receiver expose it as a chainable function: begin with
`0` and pass the previous result to continue across chunks.

## Test vectors

`vectors/generate.py` (standard-library Python, a third implementation of this
spec) writes the shared vectors in `vectors/`. Every implementation must pass
all of them in CI, and CI fails if regenerating them changes a file. Lines are
space-separated; `#` starts a comment line; byte strings are lowercase hex
(`-` for empty); numbers are decimal.

| File | Line |
|---|---|
| `crc32.txt` | `<input> <crc32 as eight hex digits>` |
| `records.txt` | `<name> <bytes> <status>` from decoding a record at the start of the bytes; `ok` continues `<consumed> <kind> <seq> <mono_us> <boot> <L>`, with the payload at bytes 32..32+L |
| `segments.txt` | `<name> <bytes> <status>` from decoding the segment header; `ok` continues `<header size> <key> <first_seq> <boot> <records> <last seq> <frontier> <stop>` |
| `batches.txt` | `<name> <bytes> <status>` from validating the whole batch; `ok` continues `<header size> <origin> <key> <hops> <N> <first seq> <last seq>`, with hops as comma-separated `node:time:transport` (`-` for none) and both seqs 0 when N is 0 |

An encoder must reproduce the bytes of every `ok` record, segment header and
batch header from its decoded fields.
