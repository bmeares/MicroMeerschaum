# Generation-scoped peer protocol, version 1

This is the draft file and transfer format for the first two-node implementation. It reuses the 32-byte `MR` record header, Pipe keys, little-endian integers, CRC-32/ISO-HDLC, and hop layout from [wire v0](wire-format.md). A stream is identified by its origin, exact Pipe key bytes and generation. The sequence in each record completes its identity. Origin and destination node IDs are unsigned 64-bit values. A generation is a nonzero, provisioned unsigned 64-bit value. An origin must preserve it with its sequence authority across restart and choose a fresh value after losing that authority. [Stream lifecycle](../docs/adr/0006-identify-stream-generations.md)

Field offsets below are zero-based. `u32` and `u64` are little-endian. `K` is the byte length of the Pipe key, including both unit separators. Valid keys occupy 1–200 bytes and satisfy the original device key grammar. CRC fields are little-endian u32 values calculated over every preceding byte of the same header. Each encoded record retains its own checksum. All reserved bytes are zero. Decoders validate the whole input and report trailing bytes.

## Saved stream identity

`MMID` binds an on-disk stream to its identity. A journal may include it in its manifest and apply that identity to the manifest's segments. Reopening must validate both the binding and the record bytes before treating a sequence as committed. A v0 archive read on its own still needs trusted origin and generation context on import.

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `MMID` |
| 4 | 1 | Version 1 |
| 5 | 3 | Reserved |
| 8 | 8 | Origin |
| 16 | 8 | Generation |
| 24 | 4 | K |
| 28 | K | Pipe key |
| 28+K | 4 | Header CRC |

## Batch

A batch carries consecutive record bytes for one generation to one final destination. A custody relay keeps the named final destination with its promise to forward the data. Rebatching can change the envelope and preserve the original records byte for byte.

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `MMBT` |
| 4 | 1 | Version 1 |
| 5 | 1 | Hop count H, 0–8 |
| 6 | 2 | Reserved |
| 8 | 8 | Origin |
| 16 | 8 | Generation |
| 24 | 8 | Final destination |
| 32 | 4 | K |
| 36 | K | Pipe key |
| 36+K | 24·H | Route, oldest hop first |
| 36+K+24·H | 4 | Record count N |
| 40+K+24·H | 4 | Header CRC |
| 44+K+24·H | variable | N complete records |

Each hop contains node ID (u64), received time in Unix microseconds (i64), transport (u8, 0–4 as in v0) and seven reserved bytes. A zero-record batch can query a configured receiver's current receipt. Its first and last record sequence are both reported as zero. A nonempty batch can begin at any positive sequence. The sequence space ends at the maximum u64. A successor never wraps to the start of the stream.

The data record payload remains opaque to the exchange. The v1 context determines how to interpret loss records. Each delivery participant must be authorized for the configured stream and role before receiving a receipt or acting on one. A checksum can detect corrupted bytes. Sender and issuer authorization belong to the authenticated communication or provisioning layer.

## Loss declaration payload

A v1 loss record retains a distinct record sequence of its own and has exactly 24 payload bytes. Offsets are relative to the start of the payload. The enclosing record CRC covers these bytes.

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 1 | Kind, 1 for missed observations, 2 for an evicted range |
| 1 | 7 | Reserved |
| 8 | 8 | Positive missed count, or first evicted sequence |
| 16 | 8 | Zero for missed observations, or last evicted sequence (inclusive) |

Missed observations had no assigned sequence, so their count accounts for no gap. An evicted range covers positive sequences in this same stream with `first <= last < declaration sequence`. An authorized declaration can account for a gap while the declaration itself is ahead of that gap. Persist the evidence before advancing the cumulative receipt and retain any original records already present in the range. A loss declaration establishes no possession of the missing bytes.

## Receipt

A single receipt covers the contiguous prefix of original records and explicitly accounted-for loss. Through zero represents an empty prefix. A receiver may retain valid records above a gap and issue a receipt for the prefix below it.

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `MMRC` |
| 4 | 1 | Version 1 |
| 5 | 1 | Role, 1 for destination, 2 for custody |
| 6 | 2 | Reserved |
| 8 | 8 | Origin |
| 16 | 8 | Generation |
| 24 | 8 | Issuer |
| 32 | 8 | Final destination |
| 40 | 8 | Through sequence |
| 48 | 4 | K |
| 52 | K | Pipe key |
| 52+K | 4 | Header CRC |

For a destination receipt the issuer equals the final destination. A custody receipt names a different issuer responsible for onward delivery. Before advancing a confirmed cursor, match the role, authorized issuer, final destination and full stream identity to the outstanding delivery. Keep the greatest confirmed prefix for that scope. A delayed receipt from a prior generation or a relay serving another archive cannot retire this delivery. The v0 HTTPS reply has its own separate legacy scope.

## Validation and fixtures

For each envelope, first require its fixed prefix (28 bytes for `MMID`, 36 for `MMBT`, 52 for `MMRC`). Then validate magic, version, tag or count, reserved bytes, key length, full header availability, header CRC, generation and role constraints, and key contents. For batches, validate hop fields before decoding records. Validate each original record under the v0 record rules, check continuity, then validate typed loss where applicable. For standalone loss payloads, validate the exact length, tag and reserved bytes, nonzero declaration sequence, then count or range. A short loss payload gives `truncated`, an oversized one gives `trailing`. Use `bad_field` for invalid generations, role combinations, hop fields and loss values, `bad_seq` for declaration sequence zero, and `seq_gap` for nonconsecutive records. Any bytes remaining after an otherwise valid envelope and its contents give `trailing`.

The [shared byte examples](../vectors/protocol-v1.txt) are generated independently in Python. Each line has a structure type, case name, hex bytes and expected decoder status. Standalone loss examples use declaration sequence 8. The C and Rust suites read the same file, including wrong-role receipts, generation zero, corrupt records, maximum sequence and explicit loss.
