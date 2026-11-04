// SPDX-License-Identifier: Apache-2.0
use std::path::Path;

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

#[test]
fn crc32_matches_shared_vectors() {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../vectors/crc32.txt");
    let text = std::fs::read_to_string(&path).expect("read crc32 vectors");
    let mut cases = 0;
    for line in text.lines() {
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let (input, want) = line.split_once(' ').expect("vector line");
        let data = decode_hex(input);
        let want = u32::from_str_radix(want.trim(), 16).expect("crc hex");
        let (head, tail) = data.split_at(data.len() / 2);
        assert_eq!(micromeerschaum::crc32(0, &data), want, "whole {input}");
        assert_eq!(
            micromeerschaum::crc32(micromeerschaum::crc32(0, head), tail),
            want,
            "split {input}"
        );
        cases += 1;
    }
    assert!(cases > 0, "no vectors in {}", path.display());
}
