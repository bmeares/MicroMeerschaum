// SPDX-License-Identifier: Apache-2.0
//! Flat document payloads (spec/document-v1.md). Strings borrow the input.

use crate::{Error, MAX_PAYLOAD};

pub const MAX_FIELDS: usize = 64;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Value<'a> {
    Null,
    Bool(bool),
    Int(i64),
    Float(f64),
    Text(&'a str),
    /// Signed Unix microseconds in UTC.
    Time(i64),
}

fn check_name(name: &str, previous: &[(&str, Value<'_>)]) -> Result<(), Error> {
    if name.is_empty()
        || name.len() > u8::MAX as usize
        || name.contains('\0')
        || previous.iter().any(|(other, _)| *other == name)
    {
        return Err(Error::BadField);
    }
    Ok(())
}

fn take<'a>(remaining: &mut &'a [u8], n: usize) -> Result<&'a [u8], Error> {
    let (head, tail) = remaining.split_at_checked(n).ok_or(Error::Truncated)?;
    *remaining = tail;
    Ok(head)
}

fn fixed<const N: usize>(remaining: &mut &[u8]) -> Result<[u8; N], Error> {
    Ok(take(remaining, N)?.try_into().expect("exact length"))
}

/// Decode the entire payload in field order, preserving empty text, null and signed zero.
pub fn decode(mut payload: &[u8]) -> Result<Vec<(&str, Value<'_>)>, Error> {
    if payload.len() > MAX_PAYLOAD {
        return Err(Error::BadLength);
    }
    let header = take(&mut payload, 6)?;
    if &header[..4] != b"MMDC" {
        return Err(Error::BadMagic);
    }
    if header[4] != 1 {
        return Err(Error::BadVersion);
    }
    let count = usize::from(header[5]);
    if count > MAX_FIELDS {
        return Err(Error::BadLength);
    }
    let mut fields = Vec::with_capacity(count);
    for _ in 0..count {
        let name_len = usize::from(fixed::<1>(&mut payload)?[0]);
        let name =
            std::str::from_utf8(take(&mut payload, name_len)?).map_err(|_| Error::BadField)?;
        check_name(name, &fields)?;
        let value = match fixed::<1>(&mut payload)?[0] {
            0 => Value::Null,
            1 => Value::Bool(false),
            2 => Value::Bool(true),
            3 => Value::Int(i64::from_le_bytes(fixed(&mut payload)?)),
            4 => {
                let real = f64::from_le_bytes(fixed(&mut payload)?);
                if !real.is_finite() {
                    return Err(Error::BadField);
                }
                Value::Float(real)
            }
            5 => {
                let len = u32::from_le_bytes(fixed(&mut payload)?) as usize;
                Value::Text(
                    std::str::from_utf8(take(&mut payload, len)?).map_err(|_| Error::BadField)?,
                )
            }
            6 => Value::Time(i64::from_le_bytes(fixed(&mut payload)?)),
            _ => return Err(Error::BadField),
        };
        fields.push((name, value));
    }
    if !payload.is_empty() {
        return Err(Error::Trailing);
    }
    Ok(fields)
}

/// Encode fields in caller order. An empty row is a six-byte document.
pub fn encode(fields: &[(&str, Value<'_>)]) -> Result<Vec<u8>, Error> {
    if fields.len() > MAX_FIELDS {
        return Err(Error::BadLength);
    }
    let mut out = b"MMDC\x01".to_vec();
    out.push(fields.len() as u8);
    for (i, &(name, value)) in fields.iter().enumerate() {
        check_name(name, &fields[..i])?;
        out.push(name.len() as u8);
        out.extend_from_slice(name.as_bytes());
        match value {
            Value::Null => out.push(0),
            Value::Bool(b) => out.push(if b { 2 } else { 1 }),
            Value::Int(n) => {
                out.push(3);
                out.extend_from_slice(&n.to_le_bytes());
            }
            Value::Float(x) => {
                if !x.is_finite() {
                    return Err(Error::BadField);
                }
                out.push(4);
                out.extend_from_slice(&x.to_le_bytes());
            }
            Value::Text(s) => {
                if s.len() > MAX_PAYLOAD.saturating_sub(out.len() + 5) {
                    return Err(Error::BadLength);
                }
                out.push(5);
                out.extend_from_slice(&(s.len() as u32).to_le_bytes());
                out.extend_from_slice(s.as_bytes());
            }
            Value::Time(t) => {
                out.push(6);
                out.extend_from_slice(&t.to_le_bytes());
            }
        }
        if out.len() > MAX_PAYLOAD {
            return Err(Error::BadLength);
        }
    }
    Ok(out)
}
