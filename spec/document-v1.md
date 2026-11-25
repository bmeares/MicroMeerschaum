# Flat document payload, version 1

A stream provisioned with profile `document-v1` carries one document per data record. Provision the same profile at the origin and materializer. For a driver-defined binary stream, provision its schema or materializer separately and retain its exact payload bytes. A driver payload may begin with the document magic. Stream configuration determines its interpretation. Loss records use their own payload format. [Record framing](protocol-v1.md)

A document begins with four ASCII bytes `MMDC`, version byte 1 and an unsigned one-byte field count (0–64). Each field then carries its name length (one byte, 1–255), the name's UTF-8 bytes, one type tag and the corresponding value below. Names must be distinct by byte comparison and exclude U+0000. UTF-8 uses RFC 3629 throughout. Encoders preserve the input field order. The maximum size of the entire payload is 65,536 bytes. A decoder validates the exact length and rejects trailing bytes, unknown versions, tags, malformed UTF-8, duplicate names and invalid scalar encodings.

| Tag | Scalar | Value bytes |
| --- | --- | --- |
| 0 | Null | Empty |
| 1 | Boolean false | Empty |
| 2 | Boolean true | Empty |
| 3 | Signed integer | Eight-byte little-endian two’s complement |
| 4 | Finite real | IEEE 754 binary64, little-endian, including either signed zero |
| 5 | Text | Four-byte little-endian unsigned byte length, then UTF-8. Empty text and U+0000 are valid. |
| 6 | UTC timestamp | Signed 64-bit little-endian Unix microseconds since 1970-01-01T00:00:00Z |

Keep integer ticks as integers. An origin with valid UTC can explicitly supply a timestamp. The enclosing record carries monotonic microseconds and boot ID independently. There is no field-level padding or checksum. The record CRC covers the entire payload. An empty document is six bytes and represents one observation. An empty batch supplies zero observations.

The fixture named `reading` contains integer `tick = 41` and real `celsius = 21.4`. The `scalars` fixture pins signed zero, the minimum signed integer, embedded NUL, a non-ASCII character and one microsecond before the epoch. The C and Rust encoders reproduce every valid fixture from its decoded fields. [Shared vectors](../vectors/documents-v1.txt)
