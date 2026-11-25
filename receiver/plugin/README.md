# Meerschaum receiver

This native Meerschaum 5.1.5 plugin accepts authenticated binary batches and returns a destination receipt after the raw SQL transaction commits. A separate command materializes the retained document records into ordinary Pipes. SQLite and PostgreSQL are supported raw stores.

## Install

On Linux x86-64 with glibc and Meerschaum 5.1.5 installed, download `mm_receiver-0.1.0-linux-x86_64.tar.gz` and extract it into the plugin directory used by the API and worker. The archive supplies `mm_receiver/plugin.toml` and `mm_receiver/libmm_receiver.so`. [Download the Linux plugin](https://github.com/bmeares/MicroMeerschaum/releases/download/v0.1.0/mm_receiver-0.1.0-linux-x86_64.tar.gz) · [Release source](https://github.com/bmeares/MicroMeerschaum/archive/refs/tags/v0.1.0.tar.gz)

```sh
export MRSM_PLUGINS_DIR="${MRSM_PLUGINS_DIR:-$HOME/.config/meerschaum/plugins}"
mkdir -p "$MRSM_PLUGINS_DIR"
tar -xzf mm_receiver-0.1.0-linux-x86_64.tar.gz -C "$MRSM_PLUGINS_DIR"
```

To build your own native library, unpack the release source on the host OS and architecture that runs the API. Build the plugin with Rust using its locked dependencies. The release toolchain is Rust 1.99.0. The following install commands assume Linux.

```sh
cargo build --manifest-path receiver/plugin/Cargo.toml --release --locked
export MRSM_PLUGINS_DIR="${MRSM_PLUGINS_DIR:-$HOME/.config/meerschaum/plugins}"
install -d "$MRSM_PLUGINS_DIR/mm_receiver"
install -m 755 receiver/plugin/target/release/libmm_receiver.so "$MRSM_PLUGINS_DIR/mm_receiver/"
install -m 644 receiver/plugin/plugin.toml "$MRSM_PLUGINS_DIR/mm_receiver/"
```

Use the same plugin directory and settings for the API and worker. For the onboard ESP32-S3 reading, keep the `device_identity.py` generated during board provisioning on the operator's computer. Grant the device owner the custom scope `mm:ingest` and issue a token with that scope. The API credential goes on the device. Use its public token ID in the receiver grant. [First reading](../../docs/micropython.md#collect-your-first-reading)

From the unpacked source on the operator's computer, generate the configuration for this board. Set `TOKEN_ID` to the token's public ID. Set `RAW_STORE` to an absolute SQLite file writable by the receiver or a PostgreSQL URL such as `postgresql://user@host/database?sslmode=verify-full`. Provision the database role with rights on the raw tables. Use a separate local SQLite file for the ordinary Pipe.

```sh
RAW_STORE='sqlite:///var/lib/micromeerschaum/raw.db?mode=rwc'
python3 examples/micropython/receiver_config.py "$TOKEN_ID" "$RAW_STORE" \
    --identity device_identity.py --instance sql:local > receiver-config.json
```

The generated `plugins.mm_receiver` settings use this board's exact origin, generation and destination, quoted to preserve 64-bit precision. They grant the `plugin:chip_sensor` / `temperature` stream and map integer `tick` and `celsius` fields into the ordinary `weather` / `chip_temperature` Pipe. Give a relay its own token and grants for the origins it forwards. For additional devices, extend the grants and outputs and assign each stream its own output Pipe. [Configuration generator](../../examples/micropython/receiver_config.py)

Copy `receiver-config.json` to the receiver host. Initialize storage, then start or restart the API with the deployment's TLS certificate and key.

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

pipe = mrsm.Pipe("weather", "chip_temperature", instance="sql:local")
print(pipe.get_docs(limit=10))
```

## Verify independently

With Python, `mrsm` 5.1.5 and OpenSSL available, run these from the repository root. The second command starts and removes a disposable PostgreSQL 18 Docker container. Both runs install a freshly built library into a temporary Meerschaum root, provision scoped tokens, serve TLS on loopback and validate the server certificate.

```sh
python receiver/plugin/tests/deployment.py --library receiver/plugin/target/release/libmm_receiver.so
python receiver/plugin/tests/deployment.py --library receiver/plugin/target/release/libmm_receiver.so --postgres
```

Add `--micropython /path/to/mm-unix/micropython` to exercise the first-reading application and generated configuration with the Unix MicroPython binding, a simulated integer temperature sensor, a live TLS upload and a read from the ordinary Pipe.

The test restarts the API after a lost reply, races two authorized routes, compares the retained bytes, submits a conflicting batch, and replays a correction that arrived before its original. It induces a database checkpoint failure after an ordinary row commits, locks and unlocks the destination Pipe, restores an evicted record, and verifies that a parser error leaves the raw receipt intact.
