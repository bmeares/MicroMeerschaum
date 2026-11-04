// SPDX-License-Identifier: Apache-2.0
//! MicroMeerschaum receiver. Native Rust, never FFI to the C core; the shared
//! vectors in `../vectors` keep the two implementations in agreement.

mod batch;
mod record;
mod segment;

use std::fmt;

pub use batch::{Batch, BatchHeader, Hop, Transport};
pub use record::{Kind, Record};
pub use segment::{Scan, SegmentHeader, scan_segment};

/// Wire-format version this crate implements (`spec/wire-format.md`).
pub const WIRE_VERSION: u8 = 0;
pub const RECORD_HEADER_LEN: usize = 32;
pub const MAX_PAYLOAD: usize = 65536;
pub const MAX_KEY_LEN: usize = 200;
pub const MAX_HOPS: usize = 8;

/// Why bytes failed to decode; [`Error::name`] gives the spec's status name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// The input ends inside the structure: more bytes are needed.
    Truncated,
    BadMagic,
    BadVersion,
    BadKind,
    BadLength,
    BadSeq,
    SeqGap,
    BadKey,
    BadField,
    BadCrc,
    Trailing,
}

impl Error {
    pub fn name(self) -> &'static str {
        match self {
            Error::Truncated => "truncated",
            Error::BadMagic => "bad_magic",
            Error::BadVersion => "bad_version",
            Error::BadKind => "bad_kind",
            Error::BadLength => "bad_length",
            Error::BadSeq => "bad_seq",
            Error::SeqGap => "seq_gap",
            Error::BadKey => "bad_key",
            Error::BadField => "bad_field",
            Error::BadCrc => "bad_crc",
            Error::Trailing => "trailing",
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.name())
    }
}

impl std::error::Error for Error {}

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

/// Checks a device pipe key (`connector 0x1F metric 0x1F location`) and returns it as text.
pub fn check_key(key: &[u8]) -> Result<&str, Error> {
    if key.is_empty() || key.len() > MAX_KEY_LEN {
        return Err(Error::BadKey);
    }
    let text = std::str::from_utf8(key).map_err(|_| Error::BadKey)?;
    let mut parts = text.split('\x1f');
    match (parts.next(), parts.next(), parts.next(), parts.next()) {
        (Some(connector), Some(metric), Some(_), None)
            if !connector.is_empty() && !metric.is_empty() =>
        {
            Ok(text)
        }
        _ => Err(Error::BadKey),
    }
}

fn need(buf: &[u8], len: usize) -> Result<(), Error> {
    if buf.len() < len {
        Err(Error::Truncated)
    } else {
        Ok(())
    }
}

fn bytes<const N: usize>(buf: &[u8], at: usize) -> [u8; N] {
    buf[at..at + N]
        .try_into()
        .expect("caller checked the length")
}

fn u32_at(buf: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(bytes(buf, at))
}

fn u64_at(buf: &[u8], at: usize) -> u64 {
    u64::from_le_bytes(bytes(buf, at))
}
