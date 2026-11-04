// SPDX-License-Identifier: Apache-2.0
use crate::{Error, MAX_KEY_LEN, Record, WIRE_VERSION, check_key, crc32, need, u32_at, u64_at};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SegmentHeader<'a> {
    pub key: &'a str,
    pub first_seq: u64,
    /// The device's boot counter when the segment was created.
    pub boot: u32,
}

impl<'a> SegmentHeader<'a> {
    /// Decodes the header at the start of `buf` and returns it with its size.
    pub fn decode(buf: &'a [u8]) -> Result<(Self, usize), Error> {
        need(buf, 24)?;
        if !buf.starts_with(b"MMSG") {
            return Err(Error::BadMagic);
        }
        if buf[4] != WIRE_VERSION {
            return Err(Error::BadVersion);
        }
        if buf[5..8] != [0, 0, 0] {
            return Err(Error::BadField);
        }
        let key_len = u32_at(buf, 20) as usize;
        if !(1..=MAX_KEY_LEN).contains(&key_len) {
            return Err(Error::BadKey);
        }
        let size = 28 + key_len;
        need(buf, size)?;
        if crc32(0, &buf[..size - 4]) != u32_at(buf, size - 4) {
            return Err(Error::BadCrc);
        }
        let first_seq = u64_at(buf, 8);
        if first_seq == 0 {
            return Err(Error::BadSeq);
        }
        let key = check_key(&buf[24..24 + key_len])?;
        Ok((
            SegmentHeader {
                key,
                first_seq,
                boot: u32_at(buf, 16),
            },
            size,
        ))
    }

    /// Appends the encoded header to `out`.
    pub fn encode(&self, out: &mut Vec<u8>) -> Result<(), Error> {
        check_key(self.key.as_bytes())?;
        if self.first_seq == 0 {
            return Err(Error::BadSeq);
        }
        let start = out.len();
        out.extend_from_slice(b"MMSG");
        out.extend_from_slice(&[WIRE_VERSION, 0, 0, 0]);
        out.extend_from_slice(&self.first_seq.to_le_bytes());
        out.extend_from_slice(&self.boot.to_le_bytes());
        out.extend_from_slice(&(self.key.len() as u32).to_le_bytes());
        out.extend_from_slice(self.key.as_bytes());
        let crc = crc32(0, &out[start..]);
        out.extend_from_slice(&crc.to_le_bytes());
        Ok(())
    }
}

/// The result of scanning one segment for its write frontier.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Scan<'a> {
    pub header: SegmentHeader<'a>,
    /// Bytes before the first record.
    pub header_len: usize,
    pub records: u64,
    /// `first_seq + records - 1`.
    pub last_seq: u64,
    /// Offset of the first byte after the last valid record.
    pub frontier: usize,
    /// Why the scan stopped; `None` when the data ends exactly after a record.
    pub stop: Option<Error>,
}

/// Decodes a segment header and scans the records after it. Fails only when
/// the header is invalid.
pub fn scan_segment(buf: &[u8]) -> Result<Scan<'_>, Error> {
    let (header, header_len) = SegmentHeader::decode(buf)?;
    let (mut records, mut frontier, mut stop) = (0u64, header_len, None);
    while frontier < buf.len() {
        match Record::decode(&buf[frontier..]) {
            Ok((record, _)) if record.seq != header.first_seq.wrapping_add(records) => {
                stop = Some(Error::SeqGap);
                break;
            }
            Ok((_, used)) => {
                frontier += used;
                records += 1;
            }
            Err(error) => {
                stop = Some(error);
                break;
            }
        }
    }
    let last_seq = header.first_seq.wrapping_add(records).wrapping_sub(1);
    Ok(Scan {
        header,
        header_len,
        records,
        last_seq,
        frontier,
        stop,
    })
}
