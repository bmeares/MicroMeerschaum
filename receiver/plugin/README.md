# Meerschaum receiver

This native Meerschaum 5.1.5 plugin accepts authenticated binary batches and returns a destination receipt after the raw SQL transaction commits. A separate command materializes the retained document records into ordinary Pipes. SQLite and PostgreSQL are supported raw stores.

## Install

Build on the host OS and architecture that runs the API. These commands install the library from the repository root on Linux with Rust and Meerschaum 5.1.5 installed.

```sh
cargo build --manifest-path receiver/plugin/Cargo.toml --release --locked
export MRSM_PLUGINS_DIR="${MRSM_PLUGINS_DIR:-$HOME/.config/meerschaum/plugins}"
install -d "$MRSM_PLUGINS_DIR/mm_receiver"
install -m 755 receiver/plugin/target/release/libmm_receiver.so "$MRSM_PLUGINS_DIR/mm_receiver/"
install -m 644 receiver/plugin/plugin.toml "$MRSM_PLUGINS_DIR/mm_receiver/"
```

Use the same plugin directory and settings for the API and the worker. Save settings such as the following under `plugins.mm_receiver` in the Meerschaum configuration. The JSON shown is a complete `MRSM_CONFIG` overlay. Set the raw database path to an absolute path writable by the operator processes. Use a separate SQLite file for the ordinary Pipe. PostgreSQL accepts a connection URL such as `postgresql://user@host/database?sslmode=verify-full` in `raw_store`; provision that role with rights on the raw tables.

```json
{
  "plugins": {
    "mm_receiver": {
      "destination": "22",
      "raw_store": "sqlite:///var/lib/micromeerschaum/raw.db?mode=rwc",
      "grants": [
        {"token_id": "REPLACE_WITH_TOKEN_ID", "origin": "11", "key": "sensor\u001ftemperature\u001f"}
      ],
      "outputs": [
        {
          "origin": "11", "generation": "1", "key": "sensor\u001ftemperature\u001f",
          "instance": "sql:local", "connector": "weather", "metric": "temperature",
          "columns": {"datetime": "tick"},
          "dtypes": {"tick": "int", "celsius": "float"}
        }
      ]
    }
  }
}
```

Grant the device owner the custom scope `mm:ingest` and issue its token with that scope. Copy the token's public ID into a grant for every exact origin and Pipe key it may send. Give a relay its own token and grants for the origins it forwards. Quote 64-bit identifiers in configuration to preserve their precision. Initialize storage, then start or restart the API with the deployment's TLS certificate and key.

```sh
export MRSM_CONFIG="$(cat receiver-config.json)"
mrsm mm receiver init
mrsm start api --certfile "$TLS_CERT" --keyfile "$TLS_KEY"
```

## Delivery

Send `POST /micromeerschaum/v1/batch` with `Content-Type: application/octet-stream` and `Authorization: Bearer <Meerschaum API key>`. The request contains one complete v1 batch and the successful response contains one complete binary `MMRC`. An empty batch returns the current receipt. A wrong destination or grant returns 403. Invalid wire bytes return 400, conflicting original bytes return 409, and a storage failure returns 503. Repeating a batch after an interrupted response is safe. [Batch and receipt format](../../spec/protocol-v1.md)

The transaction serializes a stream with `BEGIN IMMEDIATE` on SQLite or a stream-row lock on PostgreSQL. It retains original record frames in `mm_record.original`, plus the authenticated sender, byte-exact reported hops and inclusive range in `mm_route`. The prefix advances across original records and explicit evicted ranges. The loss record itself remains available for audit. The signed SQL sequence fields sort unsigned 64-bit sequence numbers by flipping the high bit. For manual SQL inspection, the device sequence equals the signed SQL value plus 9223372036854775808.

To import a v0 batch, explicitly set `"legacy": true` on its grant and send to `POST /micromeerschaum/v0/batch`. This uses its own archived stream namespace and returns the v0 JSON acknowledgement. Each v1 generation has independent receipt and replay state. [Legacy wire format](../../spec/wire-format.md)

## Replay

Run `mrsm mm receiver replay` once or schedule it through a Meerschaum job. Each output entry chooses the document parser and maps one exact v1 stream generation into a dedicated ordinary Pipe. Leave a binary stream without an output to retain it for a sensor-specific reader. A single source can have several separately keyed outputs. Set an optional `location` for each destination Pipe; omitting it selects the default location.

Declare the fields with `int`, `bool`, `float`, `string`, or `datetime`. Typed time fields carry signed Unix microseconds UTC. Each row must include all configured key columns with non-null values. The worker provisions a keyed upsert Pipe on its first write. A pre-existing Pipe must have matching keys and upsert enabled. Reserve each target for its mapped stream so independent origins and generations retain independent row identities. [Document encoding](../../spec/document-v1.md)

The worker applies one original sequence at a time through the durable prefix. A failed or locked write leaves the checkpoint before that sequence. If the worker stops after publishing a row and before saving the checkpoint, it repeats the same keyed upsert on restart. A late original inside an earlier declared eviction rewinds the affected output checkpoints and reapplies subsequent corrections in source order.

`mrsm mm receiver status` reports the raw prefix, retained record count, observed route-range count, and per-output applied checkpoints. An output acquires a checkpoint after its first successful step. Read materialized rows through the ordinary Pipe API.

```python
import meerschaum as mrsm

pipe = mrsm.Pipe("weather", "temperature", instance="sql:local")
print(pipe.get_docs(limit=10))
```

## Verify independently

With Python, `mrsm` 5.1.5 and OpenSSL available, run these from the repository root. The second command starts and removes a disposable PostgreSQL 18 Docker container. Both runs install a freshly built library into a temporary Meerschaum root, provision scoped tokens, serve TLS on loopback and validate the server certificate.

```sh
python receiver/plugin/tests/deployment.py --library receiver/plugin/target/release/libmm_receiver.so
python receiver/plugin/tests/deployment.py --library receiver/plugin/target/release/libmm_receiver.so --postgres
```

The test restarts the API after a lost reply, races two authorized routes, compares the retained bytes, submits a conflicting batch, and replays a correction that arrived before its original. It induces a database checkpoint failure after an ordinary row commits, locks and unlocks the destination Pipe, restores an evicted record, and verifies that a parser error leaves the raw receipt intact.
