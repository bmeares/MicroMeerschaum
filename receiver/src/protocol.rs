// SPDX-License-Identifier: Apache-2.0
//! Generation-scoped peer delivery, specified in `spec/protocol-v1.md`.
//!
//! Wire decoding checks structure and checksums. The caller must authenticate
//! a receipt's issuer and match its delivery obligation before reclaiming data.

use crate::batch::{HOP_LEN, decode_hops, decode_records, encode_hops};
use crate::{
    Error, Hop, Kind, MAX_HOPS, MAX_KEY_LEN, Record, check_key, crc32, need, u32_at, u64_at,
};

pub const VERSION: u8 = 1;

/// The same stream identity is retained when records are stored or forwarded.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Stream<'a> {
    pub origin: u64,
    pub generation: u64,
    pub key: &'a str,
}

impl<'a> Stream<'a> {
    fn check(&self) -> Result<(), Error> {
        if self.generation == 0 {
            return Err(Error::BadField);
        }
        check_key(self.key.as_bytes())?;
        Ok(())
    }

    fn from_header(buf: &'a [u8], key: &'a [u8]) -> Result<Self, Error> {
        let generation = u64_at(buf, 16);
        if generation == 0 {
            return Err(Error::BadField);
        }
        Ok(Self {
            origin: u64_at(buf, 8),
            generation,
            key: check_key(key)?,
        })
    }

    /// Decode a complete saved stream identity.
    pub fn decode(buf: &'a [u8]) -> Result<Self, Error> {
        check_prefix(buf, b"MMID", 28)?;
        if buf[5..8] != [0; 3] {
            return Err(Error::BadField);
        }
        let (key_end, end) = check_header_crc(buf, 24, 0)?;
        let stream = Self::from_header(buf, &buf[28..key_end])?;
        if end != buf.len() {
            return Err(Error::Trailing);
        }
        Ok(stream)
    }

    /// Append the saved identity, including its checksum.
    pub fn encode(&self, out: &mut Vec<u8>) -> Result<(), Error> {
        self.check()?;
        let start = out.len();
        self.write_prefix(out, b"MMID", 0);
        self.write_key(out);
        append_crc(out, start);
        Ok(())
    }

    fn write_prefix(&self, out: &mut Vec<u8>, magic: &[u8; 4], tag: u8) {
        out.extend_from_slice(magic);
        out.extend_from_slice(&[VERSION, tag, 0, 0]);
        out.extend_from_slice(&self.origin.to_le_bytes());
        out.extend_from_slice(&self.generation.to_le_bytes());
    }

    fn write_key(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(&(self.key.len() as u32).to_le_bytes());
        out.extend_from_slice(self.key.as_bytes());
    }
}

fn check_prefix(buf: &[u8], magic: &[u8; 4], fixed_len: usize) -> Result<(), Error> {
    need(buf, fixed_len)?;
    if !buf.starts_with(magic) {
        return Err(Error::BadMagic);
    }
    if buf[4] != VERSION {
        return Err(Error::BadVersion);
    }
    Ok(())
}

/// Return the end of the key and end of the header after checking its CRC.
fn check_header_crc(
    buf: &[u8],
    key_len_at: usize,
    after_key: usize,
) -> Result<(usize, usize), Error> {
    let key_len = u32_at(buf, key_len_at) as usize;
    if !(1..=MAX_KEY_LEN).contains(&key_len) {
        return Err(Error::BadKey);
    }
    let key_end = key_len_at + 4 + key_len;
    let crc_at = key_end + after_key;
    let end = crc_at + 4;
    need(buf, end)?;
    if crc32(0, &buf[..crc_at]) != u32_at(buf, crc_at) {
        return Err(Error::BadCrc);
    }
    Ok((key_end, end))
}

fn append_crc(out: &mut Vec<u8>, start: usize) {
    out.extend_from_slice(&crc32(0, &out[start..]).to_le_bytes());
}

/// Original observations and their reported route for one delivery.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Batch<'a> {
    pub stream: Stream<'a>,
    pub destination: u64,
    /// Oldest hop first.
    pub hops: Vec<Hop>,
    pub records: Vec<Record<'a>>,
}

impl<'a> Batch<'a> {
    /// Decode a whole batch, borrowing record payloads and the stream key.
    pub fn decode(buf: &'a [u8]) -> Result<Self, Error> {
        check_prefix(buf, b"MMBT", 36)?;
        let hop_count = usize::from(buf[5]);
        if hop_count > MAX_HOPS || buf[6..8] != [0; 2] {
            return Err(Error::BadField);
        }
        let (key_end, end) = check_header_crc(buf, 32, hop_count * HOP_LEN + 4)?;
        let stream = Stream::from_header(buf, &buf[36..key_end])?;
        let hops = decode_hops(&buf[key_end..end - 8])?;
        let count = u32_at(buf, end - 8);
        let records = decode_records(&buf[end..], count, validate_payload)?;
        Ok(Self {
            stream,
            destination: u64_at(buf, 24),
            hops,
            records,
        })
    }

    /// Append a batch. If validation fails, preserve the original output.
    pub fn encode(&self, out: &mut Vec<u8>) -> Result<(), Error> {
        self.stream.check()?;
        if self.hops.len() > MAX_HOPS {
            return Err(Error::BadField);
        }
        let count = u32::try_from(self.records.len()).map_err(|_| Error::BadLength)?;
        let start = out.len();
        self.stream
            .write_prefix(out, b"MMBT", self.hops.len() as u8);
        out.extend_from_slice(&self.destination.to_le_bytes());
        self.stream.write_key(out);
        encode_hops(&self.hops, out);
        out.extend_from_slice(&count.to_le_bytes());
        append_crc(out, start);

        let mut previous: Option<u64> = None;
        for record in &self.records {
            if previous.is_some_and(|seq| seq.checked_add(1) != Some(record.seq)) {
                out.truncate(start);
                return Err(Error::SeqGap);
            }
            if let Err(error) = validate_payload(record).and_then(|()| record.encode(out)) {
                out.truncate(start);
                return Err(error);
            }
            previous = Some(record.seq);
        }
        Ok(())
    }
}

fn validate_payload(record: &Record<'_>) -> Result<(), Error> {
    if record.kind == Kind::Loss {
        Loss::decode(record.seq, record.payload)?;
    }
    Ok(())
}

/// What responsibility a receiver accepted. A transport reply alone grants neither role.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Destination = 1,
    Custody = 2,
}

impl Role {
    fn check_issuer(self, issuer: u64, destination: u64) -> Result<(), Error> {
        if (self == Self::Destination) != (issuer == destination) {
            return Err(Error::BadField);
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Receipt<'a> {
    pub stream: Stream<'a>,
    pub role: Role,
    pub issuer: u64,
    pub destination: u64,
    /// Highest contiguous sequence accounted for. Zero denotes an empty prefix.
    pub through: u64,
}

impl<'a> Receipt<'a> {
    /// Decode an exact binary receipt, including the final destination for custody.
    pub fn decode(buf: &'a [u8]) -> Result<Self, Error> {
        check_prefix(buf, b"MMRC", 52)?;
        let role = match buf[5] {
            1 => Role::Destination,
            2 => Role::Custody,
            _ => return Err(Error::BadField),
        };
        if buf[6..8] != [0; 2] {
            return Err(Error::BadField);
        }
        let (key_end, end) = check_header_crc(buf, 48, 0)?;
        let issuer = u64_at(buf, 24);
        let destination = u64_at(buf, 32);
        role.check_issuer(issuer, destination)?;
        let stream = Stream::from_header(buf, &buf[52..key_end])?;
        if end != buf.len() {
            return Err(Error::Trailing);
        }
        Ok(Self {
            stream,
            role,
            issuer,
            destination,
            through: u64_at(buf, 40),
        })
    }

    /// Append the receipt in its binary wire representation.
    pub fn encode(&self, out: &mut Vec<u8>) -> Result<(), Error> {
        self.stream.check()?;
        self.role.check_issuer(self.issuer, self.destination)?;
        let start = out.len();
        self.stream.write_prefix(out, b"MMRC", self.role as u8);
        out.extend_from_slice(&self.issuer.to_le_bytes());
        out.extend_from_slice(&self.destination.to_le_bytes());
        out.extend_from_slice(&self.through.to_le_bytes());
        self.stream.write_key(out);
        append_crc(out, start);
        Ok(())
    }
}

/// Loss reported in a version-one batch. A missed sample has no assigned sequence.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Loss {
    Missed { count: u64 },
    Evicted { first: u64, last: u64 },
}

impl Loss {
    /// Decode a loss payload relative to the sequence of its enclosing record.
    pub fn decode(seq: u64, payload: &[u8]) -> Result<Self, Error> {
        need(payload, 24)?;
        if payload.len() > 24 {
            return Err(Error::Trailing);
        }
        if payload[1..8] != [0; 7] {
            return Err(Error::BadField);
        }
        Self::from_fields(seq, payload[0], u64_at(payload, 8), u64_at(payload, 16))
    }

    fn from_fields(seq: u64, tag: u8, first: u64, last: u64) -> Result<Self, Error> {
        if !matches!(tag, 1 | 2) {
            return Err(Error::BadField);
        }
        if seq == 0 {
            return Err(Error::BadSeq);
        }
        match tag {
            1 if first > 0 && last == 0 => Ok(Self::Missed { count: first }),
            2 if first > 0 && first <= last && last < seq => Ok(Self::Evicted { first, last }),
            _ => Err(Error::BadField),
        }
    }

    /// Form a payload for a loss record with the given sequence.
    pub fn encode(self, seq: u64) -> Result<[u8; 24], Error> {
        let (tag, first, last) = match self {
            Self::Missed { count } => (1, count, 0),
            Self::Evicted { first, last } => (2, first, last),
        };
        Self::from_fields(seq, tag, first, last)?;
        let mut payload = [0; 24];
        payload[0] = tag;
        payload[8..16].copy_from_slice(&first.to_le_bytes());
        payload[16..24].copy_from_slice(&last.to_le_bytes());
        Ok(payload)
    }
}
