// SPDX-License-Identifier: Apache-2.0
use std::path::Path;

use micromeerschaum::{Batch, BatchHeader, Record, SegmentHeader, crc32, scan_segment};

fn decode_hex(field: &str) -> Vec<u8> {
    if field == "-" {
        return Vec::new();
    }
    assert!(field.len().is_multiple_of(2), "odd hex field: {field}");
    (0..field.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&field[i..i + 2], 16).expect("hex byte"))
        .collect()
}

/// The space-separated fields of every vector line in `file`.
fn vectors(file: &str) -> Vec<Vec<String>> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../vectors")
        .join(file);
    let text = std::fs::read_to_string(&path).expect("read vectors");
    let lines: Vec<Vec<String>> = text
        .lines()
        .filter(|line| !line.is_empty() && !line.starts_with('#'))
        .map(|line| line.split(' ').map(str::to_owned).collect())
        .collect();
    assert!(!lines.is_empty(), "no vectors in {}", path.display());
    lines
}

fn num(field: &str) -> u64 {
    field.parse().expect("decimal field")
}

#[test]
fn crc32_matches_shared_vectors() {
    for f in vectors("crc32.txt") {
        let data = decode_hex(&f[0]);
        let want = u32::from_str_radix(&f[1], 16).expect("crc hex");
        let (head, tail) = data.split_at(data.len() / 2);
        assert_eq!(crc32(0, &data), want, "whole {}", f[0]);
        assert_eq!(crc32(crc32(0, head), tail), want, "split {}", f[0]);
    }
}

#[test]
fn records_match_shared_vectors() {
    for f in vectors("records.txt") {
        let data = decode_hex(&f[1]);
        let (record, used) = match Record::decode(&data) {
            Ok(decoded) => decoded,
            Err(error) => {
                assert_eq!(error.name(), f[2], "{}", f[0]);
                continue;
            }
        };
        assert_eq!(f[2], "ok", "{}", f[0]);
        let got = [
            used as u64,
            record.kind as u64,
            record.seq,
            record.mono_us,
            u64::from(record.boot),
        ];
        let want: Vec<u64> = f[3..8].iter().map(|x| num(x)).collect();
        assert_eq!(got[..], want[..], "{}", f[0]);
        assert_eq!(
            record.payload,
            &data[32..32 + num(&f[8]) as usize],
            "{}",
            f[0]
        );
        let mut encoded = Vec::new();
        record.encode(&mut encoded).expect("encode");
        assert_eq!(encoded, data[..used], "{}", f[0]);
    }
}

#[test]
fn segments_match_shared_vectors() {
    for f in vectors("segments.txt") {
        let data = decode_hex(&f[1]);
        let scan = match scan_segment(&data) {
            Ok(scan) => scan,
            Err(error) => {
                assert_eq!(error.name(), f[2], "{}", f[0]);
                continue;
            }
        };
        assert_eq!(f[2], "ok", "{}", f[0]);
        assert_eq!(scan.header_len as u64, num(&f[3]), "{}", f[0]);
        assert_eq!(scan.header.key.as_bytes(), decode_hex(&f[4]), "{}", f[0]);
        let got = [
            scan.header.first_seq,
            u64::from(scan.header.boot),
            scan.records,
            scan.last_seq,
            scan.frontier as u64,
        ];
        let want: Vec<u64> = f[5..10].iter().map(|x| num(x)).collect();
        assert_eq!(got[..], want[..], "{}", f[0]);
        assert_eq!(
            scan.stop.map_or("end", |stop| stop.name()),
            f[10],
            "{}",
            f[0]
        );
        let mut encoded = Vec::new();
        scan.header.encode(&mut encoded).expect("encode");
        assert_eq!(encoded, data[..scan.header_len], "{}", f[0]);
        assert_eq!(
            SegmentHeader::decode(&data),
            Ok((scan.header, scan.header_len))
        );
    }
}

#[test]
fn batches_match_shared_vectors() {
    for f in vectors("batches.txt") {
        let data = decode_hex(&f[1]);
        let batch = match Batch::decode(&data) {
            Ok(batch) => batch,
            Err(error) => {
                assert_eq!(error.name(), f[2], "{}", f[0]);
                continue;
            }
        };
        assert_eq!(f[2], "ok", "{}", f[0]);
        let (header, header_len) = BatchHeader::decode(&data).expect("header");
        assert_eq!(header, batch.header);
        assert_eq!(header_len as u64, num(&f[3]), "{}", f[0]);
        assert_eq!(header.origin, num(&f[4]), "{}", f[0]);
        assert_eq!(header.key.as_bytes(), decode_hex(&f[5]), "{}", f[0]);
        let hops: Vec<String> = header
            .hops
            .iter()
            .map(|hop| format!("{}:{}:{}", hop.node, hop.received_us, hop.transport as u8))
            .collect();
        let hops = if hops.is_empty() {
            "-".to_owned()
        } else {
            hops.join(",")
        };
        assert_eq!(hops, f[6], "{}", f[0]);
        assert_eq!(u64::from(header.count), num(&f[7]), "{}", f[0]);
        assert_eq!(batch.records.len() as u64, num(&f[7]), "{}", f[0]);
        assert_eq!(
            batch.seq_range().unwrap_or((0, 0)),
            (num(&f[8]), num(&f[9])),
            "{}",
            f[0]
        );
        let mut encoded = Vec::new();
        header.encode(&mut encoded).expect("encode header");
        for record in &batch.records {
            record.encode(&mut encoded).expect("encode record");
        }
        assert_eq!(encoded, data, "{}", f[0]);
    }
}
