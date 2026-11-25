use micromeerschaum::protocol::{Batch, Receipt, Stream};
use micromeerschaum::{Kind, Record};
use mm_receiver::Receiver;
use serde_json::json;

#[test]
fn an_authorized_delivery_survives_reopen() {
    let dir = tempfile::tempdir().unwrap();
    let config = json!({
        "raw_store": format!("sqlite://{}?mode=rwc", dir.path().join("raw.db").display()),
        "destination": 22,
        "grants": [{"token_id":"direct","origin":11,"key":"sensor\u{1f}reading\u{1f}"}]
    });
    let stream = Stream {
        origin: 11,
        generation: 1,
        key: "sensor\u{1f}reading\u{1f}",
    };
    let mut batch = Batch {
        stream,
        destination: 22,
        hops: vec![],
        records: vec![Record {
            seq: 1,
            kind: Kind::Data,
            boot: 1,
            mono_us: 1,
            payload: b"first",
        }],
    };
    let mut body = vec![];
    batch.encode(&mut body).unwrap();
    let open = || Receiver::from_config(config.clone()).unwrap();
    open().initialize().unwrap();
    let reply = open().receive("direct", &body, false).unwrap();
    assert_eq!(Receipt::decode(&reply).unwrap().through, 1);
    batch.records.clear();
    body.clear();
    batch.encode(&mut body).unwrap();
    assert_eq!(open().receive("direct", &body, false).unwrap(), reply);
}
