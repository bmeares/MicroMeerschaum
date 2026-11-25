// SPDX-License-Identifier: Apache-2.0
use micromeerschaum::protocol::{Batch, Loss, Receipt, Role, Stream};
use micromeerschaum::{Error, Hop, Kind, Record, Transport};

fn reencode(kind: &str, wire: &[u8], out: &mut Vec<u8>) -> Result<(), Error> {
    match kind {
        "stream" => Stream::decode(wire)?.encode(out),
        "batch" => Batch::decode(wire)?.encode(out),
        "receipt" => Receipt::decode(wire)?.encode(out),
        "loss" => {
            out.extend_from_slice(&Loss::decode(8, wire)?.encode(8)?);
            Ok(())
        }
        _ => panic!("unknown fixture kind {kind}"),
    }
}

#[test]
fn matches_shared_protocol_vectors() {
    let mut checked = 0;
    let vectors = std::fs::read_to_string(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../vectors/protocol-v1.txt"
    ))
    .expect("read shared protocol fixtures");
    for line in vectors
        .lines()
        .filter(|line| !line.starts_with('#') && !line.is_empty())
    {
        let mut fields = line.split_whitespace();
        let kind = fields.next().expect("kind");
        let name = fields.next().expect("name");
        let hex = fields.next().expect("hex");
        let status = fields.next().expect("status");
        let wire: Vec<u8> = if hex == "-" {
            Vec::new()
        } else {
            assert!(hex.len().is_multiple_of(2), "{name}: odd hex length");
            (0..hex.len())
                .step_by(2)
                .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).expect("hex byte"))
                .collect()
        };
        let mut out = vec![0x55, 0xaa];
        let result = reencode(kind, &wire, &mut out);
        assert_eq!(
            result.map_or_else(|e| e.name(), |()| "ok"),
            status,
            "{name}"
        );
        if status == "ok" {
            assert_eq!(&out[2..], wire, "{name}: encode");
            // A shared encode/decode field swap would still round-trip the bytes.
            let expected_stream = Stream {
                origin: 0x0123_4567_89ab_cdef,
                generation: 0xfedc_ba98_7654_3210,
                key: "sensor\x1ftemperature\x1f",
            };
            match (kind, name) {
                ("receipt", "custody") => assert_eq!(
                    Receipt::decode(&wire).unwrap(),
                    Receipt {
                        stream: expected_stream,
                        role: Role::Custody,
                        issuer: 17,
                        destination: 4321,
                        through: 6,
                    }
                ),
                ("batch", "two_equal_readings") => {
                    let batch = Batch::decode(&wire).unwrap();
                    assert_eq!(batch.stream, expected_stream);
                    assert_eq!(batch.destination, 4321);
                    assert_eq!(
                        batch.hops,
                        [Hop {
                            node: 15,
                            received_us: -99,
                            transport: Transport::Ble
                        }]
                    );
                    assert_eq!((batch.records[0].seq, batch.records[1].seq), (42, 43));
                    assert_eq!(batch.records[0].payload, b"degree:19");
                    assert_eq!(batch.records[0].payload, batch.records[1].payload);
                }
                _ => {}
            }
            for end in 0..wire.len() {
                assert!(
                    reencode(kind, &wire[..end], &mut Vec::new()).is_err(),
                    "{name}: accepted truncated frame ending at {end}"
                );
            }
        }
        checked += 1;
    }
    assert!(checked > 0);
}

#[test]
fn invalid_records_leave_preceding_output_intact() {
    let stream = Stream {
        origin: 1,
        generation: 2,
        key: "sensor\x1ftemperature\x1f",
    };
    let record = Record {
        kind: Kind::Data,
        seq: u64::MAX,
        mono_us: 0,
        boot: 0,
        payload: &[],
    };
    let mut batch = Batch {
        stream,
        destination: 3,
        hops: Vec::new(),
        records: vec![record],
    };
    let mut wire = Vec::new();
    batch.encode(&mut wire).unwrap();
    assert_eq!(Batch::decode(&wire).unwrap(), batch);
    let expected = wire.clone();
    batch.records.push(Record { seq: 1, ..record });
    assert_eq!(batch.encode(&mut wire), Err(Error::SeqGap));
    assert_eq!(wire, expected);

    batch.records = vec![
        Record { seq: 1, ..record },
        Record {
            kind: Kind::Loss,
            seq: 2,
            ..record
        },
    ];
    assert_eq!(batch.encode(&mut wire), Err(Error::Truncated));
    assert_eq!(wire, expected);
}

#[test]
fn loss_and_custody_validate_their_scope() {
    assert_eq!(
        Loss::Evicted { first: 1, last: 8 }.encode(8),
        Err(Error::BadField)
    );
    assert_eq!(Loss::Missed { count: 1 }.encode(0), Err(Error::BadSeq));
    let receipt = Receipt {
        stream: Stream {
            origin: 1,
            generation: 2,
            key: "sensor\x1fstate\x1f",
        },
        role: Role::Custody,
        issuer: 3,
        destination: 4,
        through: 0,
    };
    let mut wire = Vec::new();
    receipt.encode(&mut wire).unwrap();
    assert_eq!(Receipt::decode(&wire), Ok(receipt));
    let expected = wire.clone();
    assert_eq!(
        Receipt {
            role: Role::Destination,
            ..receipt
        }
        .encode(&mut wire),
        Err(Error::BadField)
    );
    assert_eq!(wire, expected);
}
