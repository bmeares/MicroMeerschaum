// SPDX-License-Identifier: Apache-2.0
//! MicroMeerschaum receiver. Native Rust, never FFI to the C core; the shared
//! vectors in `../vectors` keep the two implementations in agreement.

/// Wire-format version this crate implements (`spec/wire-format.md`).
pub const WIRE_VERSION: u8 = 0;

/// CRC-32/ISO-HDLC, chainable like zlib's `crc32()`: start with 0 and pass the
/// previous result to continue over the next chunk.
pub fn crc32(crc: u32, data: &[u8]) -> u32 {
    let mut crc = !crc;
    for &byte in data {
        crc ^= u32::from(byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xEDB8_8320 & 0u32.wrapping_sub(crc & 1));
        }
    }
    !crc
}
