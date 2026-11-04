// SPDX-License-Identifier: Apache-2.0
use crate::{Error, MAX_PAYLOAD, RECORD_HEADER_LEN, WIRE_VERSION, crc32, need, u32_at, u64_at};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// One document of the device pipe, such as a raw sensor frame.
    Data = 0,
    /// UTF-8 JSON describing data the device knows it lost.
    Loss = 1,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Record<'a> {
    pub kind: Kind,
    pub seq: u64,
    /// Microseconds since the boot that wrote the record.
    pub mono_us: u64,
    pub boot: u32,
    pub payload: &'a [u8],
}

impl<'a> Record<'a> {
    /// Decodes the record at the start of `buf` and returns it with the number
    /// of bytes it occupies. [`Error::Truncated`] means more bytes are needed.
    pub fn decode(buf: &'a [u8]) -> Result<(Self, usize), Error> {
        need(buf, RECORD_HEADER_LEN)?;
        if !buf.starts_with(b"MR") {
            return Err(Error::BadMagic);
        }
        if buf[2] != WIRE_VERSION {
            return Err(Error::BadVersion);
        }
        let kind = match buf[3] {
            0 => Kind::Data,
            1 => Kind::Loss,
            _ => return Err(Error::BadKind),
        };
        let len = u32_at(buf, 4) as usize;
        if len > MAX_PAYLOAD {
            return Err(Error::BadLength);
        }
        let end = RECORD_HEADER_LEN + len;
        need(buf, end)?;
        let payload = &buf[RECORD_HEADER_LEN..end];
        if crc32(crc32(0, &buf[..28]), payload) != u32_at(buf, 28) {
            return Err(Error::BadCrc);
        }
        let seq = u64_at(buf, 8);
        if seq == 0 {
            return Err(Error::BadSeq);
        }
        let record = Record {
            kind,
            seq,
            mono_us: u64_at(buf, 16),
            boot: u32_at(buf, 24),
            payload,
        };
        Ok((record, end))
    }

    /// The 32-byte header alone, for a payload stored elsewhere.
    pub fn header(&self) -> Result<[u8; RECORD_HEADER_LEN], Error> {
        if self.payload.len() > MAX_PAYLOAD {
            return Err(Error::BadLength);
        }
        if self.seq == 0 {
            return Err(Error::BadSeq);
        }
        let mut header = [0u8; RECORD_HEADER_LEN];
        header[..4].copy_from_slice(&[b'M', b'R', WIRE_VERSION, self.kind as u8]);
        header[4..8].copy_from_slice(&(self.payload.len() as u32).to_le_bytes());
        header[8..16].copy_from_slice(&self.seq.to_le_bytes());
        header[16..24].copy_from_slice(&self.mono_us.to_le_bytes());
        header[24..28].copy_from_slice(&self.boot.to_le_bytes());
        let crc = crc32(crc32(0, &header[..28]), self.payload);
        header[28..].copy_from_slice(&crc.to_le_bytes());
        Ok(header)
    }

    /// Appends the encoded record to `out`.
    pub fn encode(&self, out: &mut Vec<u8>) -> Result<(), Error> {
        out.extend_from_slice(&self.header()?);
        out.extend_from_slice(self.payload);
        Ok(())
    }
}
