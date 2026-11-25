# BLE GATT exchange

The central pushes one v1 batch to a provisioned peripheral and reads one binary receipt. Either runtime can take either role. The application authenticates the remote identity before exposing a journal or accepting its receipt. Each connection has its own assembly state and bounded buffers. Discard partial state on disconnect. A reconnection retries the original immutable records.

The service UUID is `d9da0001-5af1-49e5-9d3a-825119b55722`. The write-with-response characteristic uses the same UUID ending in the first group's `0002`, and the read characteristic uses `0003`. All offsets and sizes below are unsigned little-endian 32-bit integers. A fragment has a nine-byte header.

| Op | Header after opcode | Data |
| --- | --- | --- |
| PUSH (1), write | fragment offset, total batch bytes | One or more batch bytes. Offset zero starts a new transfer. Offsets must be contiguous. |
| SEEK (2), write | reply offset, maximum characteristic value size | Empty. Requests the reply fragment at that offset. |
| REPLY (3), read | reply offset, total receipt bytes | One or more receipt bytes, bounded by the requested value size. |

Use the smaller of the characteristic limit and the negotiated ATT MTU minus three bytes. At minimum MTU 23, each PUSH carries up to eleven application bytes. The last PUSH invokes the receiver commit. A failed commit yields no receipt. The central reassembles the complete receipt before confirming it against the stream, generation, issuer and destination. ATT write responses indicate fragment acceptance. Only a verified MicroMeerschaum receipt can authorize reclamation. [C interface](../core/include/micromeerschaum/gatt.h)
