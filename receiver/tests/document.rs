// SPDX-License-Identifier: Apache-2.0
use micromeerschaum::document::{Value, decode, encode};

#[test]
fn reads_and_writes_shared_document_fixtures() {
    for line in include_str!("../../vectors/documents-v1.txt").lines() {
        if line.starts_with('#') {
            continue;
        }
        let mut words = line.split_whitespace();
        let name = words.next().unwrap();
        let hex = words.next().unwrap().as_bytes();
        let status = words.next().unwrap();
        let payload: Vec<_> = hex
            .as_chunks::<2>()
            .0
            .iter()
            .map(|s| u8::from_str_radix(std::str::from_utf8(s).unwrap(), 16).unwrap())
            .collect();
        match decode(&payload) {
            Ok(fields) => {
                assert_eq!(status, "ok", "{name}");
                if name == "reading" {
                    assert_eq!(
                        fields,
                        [("tick", Value::Int(41)), ("celsius", Value::Float(21.4))]
                    );
                }
                if name == "scalars" {
                    assert_eq!(
                        fields,
                        [
                            ("n", Value::Null),
                            ("f", Value::Bool(false)),
                            ("t", Value::Bool(true)),
                            ("i", Value::Int(i64::MIN)),
                            ("x", Value::Float(-0.0)),
                            ("s", Value::Text("a\0é")),
                            ("d", Value::Time(-1))
                        ]
                    );
                }
                assert_eq!(encode(&fields).unwrap(), payload, "{name}");
            }
            Err(error) => assert_eq!(error.name(), status, "{name}"),
        }
    }
}

#[test]
fn refuses_unrepresentable_documents() {
    assert!(encode(&[("x", Value::Null), ("x", Value::Bool(true))]).is_err());
    assert!(encode(&[("x", Value::Float(f64::INFINITY))]).is_err());
    assert!(encode(&[("x", Value::Text(&"a".repeat(65536)))]).is_err());
}
