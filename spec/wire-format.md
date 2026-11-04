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

The batch, ack and lineage encodings are not yet specified.

## Conventions

- Multi-byte integers are little-endian.
- Every encoded structure starts with the wire version byte. A receiver rejects
  versions it does not implement.

## Integrity

Checksums are CRC-32/ISO-HDLC: polynomial `0x04C11DB7` (reflected
`0xEDB88320`), initial value `0xFFFFFFFF`, reflected input and output, final XOR
`0xFFFFFFFF`. The check value of the ASCII string `123456789` is `0xCBF43926`.
The C core and the Rust receiver expose it as a chainable function: begin with
`0` and pass the previous result to continue across chunks.

## Test vectors

`vectors/` holds the shared vectors. Every implementation must pass all of them
in CI.

| File | Format |
|---|---|
| `crc32.txt` | One case per line: input as lowercase hex (`-` for empty), a space, the CRC as eight hex digits. `#` starts a comment line. |
