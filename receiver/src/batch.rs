// SPDX-License-Identifier: Apache-2.0
use crate::{
    Error, MAX_HOPS, MAX_KEY_LEN, Record, WIRE_VERSION, bytes, check_key, crc32, need, u32_at,
    u64_at,
};

const HOP_LEN: usize = 24;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Transport {
    Local = 0,
    Ble = 1,
    Wifi = 2,
    Lte = 3,
    Usb = 4,
}

impl Transport {
    fn from_byte(byte: u8) -> Option<Self> {
        Some(match byte {
            0 => Transport::Local,
            1 => Transport::Ble,
            2 => Transport::Wifi,
            3 => Transport::Lte,
            4 => Transport::Usb,
            _ => return None,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Hop {
    pub node: u64,
    /// Unix microseconds.
    pub received_us: i64,
    pub transport: Transport,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BatchHeader<'a> {
    /// Origin device id (Walter uses its 48-bit MAC).
    pub origin: u64,
    pub key: &'a str,
    /// Oldest first.
    pub hops: Vec<Hop>,
    /// Records that follow the header.
    pub count: u32,
}

impl<'a> BatchHeader<'a> {
    /// Decodes the header at the start of `buf` and returns it with its size.
    pub fn decode(buf: &'a [u8]) -> Result<(Self, usize), Error> {
        need(buf, 20)?;
        if !buf.starts_with(b"MMBT") {
            return Err(Error::BadMagic);
        }
        if buf[4] != WIRE_VERSION {
            return Err(Error::BadVersion);
        }
        let hop_count = usize::from(buf[5]);
        if hop_count > MAX_HOPS || buf[6..8] != [0, 0] {
            return Err(Error::BadField);
        }
        let key_len = u32_at(buf, 16) as usize;
        if !(1..=MAX_KEY_LEN).contains(&key_len) {
            return Err(Error::BadKey);
        }
        let size = 28 + key_len + HOP_LEN * hop_count;
        need(buf, size)?;
        if crc32(0, &buf[..size - 4]) != u32_at(buf, size - 4) {
            return Err(Error::BadCrc);
        }
        let key = check_key(&buf[20..20 + key_len])?;
        let hops = buf[20 + key_len..size - 8]
            .as_chunks::<HOP_LEN>()
            .0
            .iter()
            .map(|hop| {
                let transport = Transport::from_byte(hop[16]).ok_or(Error::BadField)?;
                if hop[17..] != [0; 7] {
                    return Err(Error::BadField);
                }
                Ok(Hop {
                    node: u64_at(hop, 0),
                    received_us: i64::from_le_bytes(bytes(hop, 8)),
                    transport,
                })
            })
            .collect::<Result<Vec<_>, _>>()?;
        let header = BatchHeader {
            origin: u64_at(buf, 8),
            key,
            hops,
            count: u32_at(buf, size - 8),
        };
        Ok((header, size))
    }

    /// Appends the encoded header to `out`.
    pub fn encode(&self, out: &mut Vec<u8>) -> Result<(), Error> {
        check_key(self.key.as_bytes())?;
        if self.hops.len() > MAX_HOPS {
            return Err(Error::BadField);
        }
        let start = out.len();
        out.extend_from_slice(b"MMBT");
        out.extend_from_slice(&[WIRE_VERSION, self.hops.len() as u8, 0, 0]);
        out.extend_from_slice(&self.origin.to_le_bytes());
        out.extend_from_slice(&(self.key.len() as u32).to_le_bytes());
        out.extend_from_slice(self.key.as_bytes());
        for hop in &self.hops {
            out.extend_from_slice(&hop.node.to_le_bytes());
            out.extend_from_slice(&hop.received_us.to_le_bytes());
            out.extend_from_slice(&[hop.transport as u8, 0, 0, 0, 0, 0, 0, 0]);
        }
        out.extend_from_slice(&self.count.to_le_bytes());
        let crc = crc32(0, &out[start..]);
        out.extend_from_slice(&crc.to_le_bytes());
        Ok(())
    }
}

/// A validated batch: its header and exactly `header.count` records with consecutive seq.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Batch<'a> {
    pub header: BatchHeader<'a>,
    pub records: Vec<Record<'a>>,
}

impl<'a> Batch<'a> {
    /// Validates a whole batch buffer.
    pub fn decode(buf: &'a [u8]) -> Result<Self, Error> {
        let (header, mut offset) = BatchHeader::decode(buf)?;
        let mut records: Vec<Record<'a>> = Vec::new();
        for _ in 0..header.count {
            let (record, used) = Record::decode(&buf[offset..])?;
            if records
                .last()
                .is_some_and(|last| record.seq != last.seq.wrapping_add(1))
            {
                return Err(Error::SeqGap);
            }
            records.push(record);
            offset += used;
        }
        if offset != buf.len() {
            return Err(Error::Trailing);
        }
        Ok(Batch { header, records })
    }

    /// First and last seq, or `None` for an empty batch.
    pub fn seq_range(&self) -> Option<(u64, u64)> {
        Some((self.records.first()?.seq, self.records.last()?.seq))
    }
}
