# Syncing Pipes between nodes

Two provisioned nodes can exchange a Pipe while disconnected from the internet. A node can originate readings, be the destination for a peer’s readings, or forward a stream to another peer or a Meerschaum instance. Applications construct a network by configuring these relationships for their Pipes. [Project scope](adr/0001-support-independent-meerschaum-deployments.md) · [Peer destinations](adr/0011-allow-peer-destinations.md)

## What a Pipe user needs to know

For an offline collector, create a local Pipe and configure its delivery destination. Call `pipe.sync(df)` to save supplied readings, or `pipe.sync()` to collect from its source plugin. A successful local call means the node can reopen the readings after a restart. Check delivery separately to see whether the named peer has accepted them. When delivery resumes after a disconnect, the framework retries from the last confirmed position. If the local journal fills or the Pipe reports loss, surface that outcome to the application. [MicroPython Pipe conventions](adr/0009-make-micropython-and-source-plugins-first-class.md) · [Retention](adr/0002-configure-retention-per-device-pipe.md)

The framework assigns identities, tracks receipts and recognizes retries. A sensor plugin can supply readings through a single `fetch` hook. A deployment that allows a relay to take responsibility for undelivered records also needs to show which relay holds them and whether the final destination has received them. [Relay custody](adr/0005-support-ble-custody-and-micropython.md)

### Capture now, send on the application's schedule

When a driver already has readings, give it a push Pipe. When a plugin owns acquisition, select its named connector and let the Pipe ask it for data. MicroPython and native ESP-IDF provide both operations. The examples below use MicroPython. [Equivalent native application](device-api.md#one-temperature-pipe-in-two-environments)

```python
# A driver supplies two rows to a local sink.
readings = Pipe("sensor", "temperature")
readings.sync({"sample": [41, 42], "celsius": [21.4, 21.4]})

# A configured source plugin acquires its own rows.
weather = Pipe("plugin:weather", "observations")
weather.sync()
```

The local writer can group observations into a durable commit while the radio sleeps. A timing-sensitive driver can enqueue to RAM and let a writer commit off the acquisition path. Enqueue success describes buffered data. A successful local `Pipe.sync(df)` describes committed data. The input frame, the local commit group and the transfer batch can each have a different size.

For example, an application can wake the radio every 15 minutes, connect once to `api:cloud`, and send as many byte-bounded batches of committed records as its radio budget allows. A missed window leaves the backlog on the device. On reconnect the sender resumes from the destination’s confirmed sequence and can regroup records. The application chooses the interval and wake policy. A peer reached over BLE can consume the same journal when the nodes meet. Measure acquisition stall and durable commit latency locally, then radio wakeups, round-trips, radio-on time and backlog drained per wake on the device.

The closest desktop action is `mrsm copy pipes sql:dest -i sql:src --sync-data incremental -y`. It copies data in chunks and can keep an existing destination’s registration. Device delivery uses a similar incremental workflow. Its progress is the last contiguous sequence the receiver has durably accounted for, so a late reading still travels even if its event time precedes the latest uploaded row. [Meerschaum copy action](https://github.com/bmeares/Meerschaum/blob/v5.1.5/docs/zensical/reference/actions/index.md#pipe-lifecycle) · [Implementation of incremental copy](https://github.com/bmeares/Meerschaum/blob/v5.1.5/crates/meerschaum-cli/src/native/manage/copy.rs#L428-L450)

In the desktop SQLite reference case, a successful incremental copy left an old correction outside its backtrack window. In the device acceptance test, make that same correction locally after newer readings have reached the destination. Its new outgoing sequence must deliver it on the next wake, and replay must leave the corrected value in the ordinary Pipe. [Reference case and expected results](meerschaum-design.md#copying-rows-and-forwarding-records)

### Where does a Pipe live?

In Meerschaum, the source connector supplies data and the instance connector gives access to the Pipe’s stored rows and metadata. On a device, an ordinary collection Pipe can use the default local journal. Select `instance="sql:local"` on a supported board when the application needs to read or update local rows. Both choices let the device collect while disconnected. [Meerschaum glossary](https://github.com/bmeares/Meerschaum/blob/v5.1.5/GLOSSARY.md#pipes-and-data) · [On-device SQLite](adr/0010-offer-an-optional-on-device-sqlite-instance.md)

In MicroPython, select a provisioned peer instance in the Pipe constructor to write directly to that peer. This example supplies a frame as columns.

```python
from micromeerschaum import Pipe

temperature = Pipe("sensor", "temperature", instance="peer:greenhouse")
temperature.sync({"tick": [1, 2], "celsius": [21.4, 21.7]})
```

Here `peer:greenhouse` identifies a configured peer reached over BLE. Constructing the handle and reading its keys should take zero radio exchanges. On a provisioned Pipe, send the key and batch together. The peer resolves the key and validates the write locally, then returns the result. An unknown or incompatible definition needs an explicit response. Registration, a metadata change, or an explicit request for current remote metadata can justify a separate exchange. Test a known Pipe with both nodes’ metadata caches cold and count application-level requests separately from BLE fragmentation. [Desktop Pipe construction](https://github.com/bmeares/Meerschaum/blob/v5.1.5/meerschaum/core/Pipe/__init__.py#L400-L427)

Success describes the selected target. For a peer journal it confirms the supplied records are durable on that peer. For a peer Pipe offering queryable rows, success requires the peer to commit the rows. The same distinction applies to a cloud receiver’s raw receipt and its later materialization. If a peer sleeps or its reply is lost, report remote completion as unconfirmed. If the sender durably staged the records, identify them as pending and retry the same identities. The offline collection example below uses a locally bound Pipe so it can succeed while the destination sleeps. Remote reads can be added for a peer that exposes queryable rows.

Meerschaum’s `source.copy_to(instance_keys)` addresses a different starting point. The source Pipe already contains data. Desktop Meerschaum copies the parameters, registers or edits the destination Pipe, then syncs chunks of source rows. Device forwarding also starts from existing data but carries the original journal identities and advances a durable receipt. Keep routine forwarding under the configured delivery. Reserve `copy_to()` for an explicit Pipe copy that can honor its metadata and data contract on the target. [Desktop copy implementation](https://github.com/bmeares/Meerschaum/blob/v5.1.5/meerschaum/core/Pipe/_copy.py#L58-L102)

## Initial release example

A greenhouse sensor A and a controller B have a configured delivery for the temperature Pipe. B is the destination. The acceptance example uses only the framework’s public interfaces and an ordinary document payload.

1. On A, a source plugin returns a reading such as `{"temperature_c": 21.4}`. A successful `Pipe.sync()` commits the observation to A’s selected local target. B may be offline at this point.
2. When the nodes meet over BLE, A delivers committed records after B’s last confirmed position for that origin, Pipe key and generation. B persists the records with their original identity and records the observed route.
3. Interrupt the connection after B commits and before A receives the reply. Restart B and reconnect. B can report the durable prefix it already holds. If A resends the same records, B recognizes their identities, retains one copy of each reading and returns its receipt.
4. Append another reading on A. Verify that B receives it at the next sequence, that B can read the original records, and that A’s delivery cursor advances only after B’s receipt. Repeat with the node roles reversed using B’s own source stream.

The native and MicroPython public interfaces implement this exchange with the bounded device journal and GATT. The mixed-runtime host test restarts both endpoints after a lost receipt in each role assignment. [Host journal](host-journal.md) · [Generation-scoped protocol](../spec/protocol-v1.md) · [Device sync convention](adr/0009-make-micropython-and-source-plugins-first-class.md)

## Configure offline relay custody

For A=1 → B=2 → D=9, provision the same origin, Pipe key, generation and final destination in three local images. In MicroPython, A sets `custodian=2`, B sets `receiver=True, custodian=2`, and D sets `receiver=True`. In native C the corresponding mode and role pairs are origin/custody, receiver/custody and receiver/destination. The first two use issuer 2 and the last uses issuer 9. Authenticate and authorize the immediate peer for the provisioned stream on each connection.

| Node | `prefix` | `confirmed` | Reclamation |
| --- | --- | --- | --- |
| A | Locally committed | B's durable custody receipt | A can release complete segments through this cursor. |
| B | Accepted custody | D's durable destination receipt, capped at B's contiguous prefix | B protects all pending originals under either retention policy. |
| D | Durably accounted for at the destination | Unused | D retains the received records. |

B builds onward batches using `pipe.batch()` or `mm_peer_batch()` on its incoming journal. It appends itself to an observed path, splitting the batch at a path boundary as necessary. The original record bytes, sequence, stream generation and final destination travel unchanged. A route-only arrival leaves the outgoing sequence cursor in place. The current BLE forwarding profile marks the new hop as BLE with timestamp zero (unknown). `pipe.routes()` returns each observed interval as `(first, last, authenticated_sender, reported_node_ids)`. Native `mm_journal_route_get()` also supplies the reported hop times and transports.

The MicroPython BLE example provides `open_role`, `receive_window` and `send_window`. For example, after mounting its volume at `/mm` and commissioning fixed-address authenticated bonds, B can run the following whenever the respective peers are available. Supply the enrolled aioble Device objects for A and D. An interrupted window leaves B's journal ready for the next connection. [MicroPython BLE example](../examples/micropython/offline_relay.py)

```python
import asyncio
from offline_relay import open_role, receive_window, send_window

relay = open_role("relay", "/mm/RELAY.MM")
asyncio.run(receive_window(relay, origin_device, sender=1))
asyncio.run(send_window(relay, destination_device, issuer=9))
```

A runs `open_role("origin", ...)`, collects with `pipe.sync({"temperature_c": 21.4})`, then calls `send_window(pipe, relay_device, 2)`. D runs `receive_window(destination, relay_device, 2)`. Set `create=True` when commissioning a new image.

The ESP-IDF example supplies the same roles for an application that owns GAP, bonding and its filesystem mount. Enable NimBLE and `CONFIG_MICROMEERSCHAUM_NIMBLE`, add the example source and include path to the application component, and open the journal with the appropriate mode and role. Keep `peer_role` in static storage. Call `peer_role_init()` after mounting. A peripheral calls `peer_role_register()` with the upstream enrollment after NimBLE initialization and before host startup. Advertise the peer service UUID. On each GAP disconnect call `peer_role_disconnect()` from the host callback. The example checks the bonded identity address, encryption and authentication for each operation. A central calls `peer_role_deliver()` in an application task after connecting and securing the link. Close a failed connection before retrying. Serialize local journal operations with the role's journal lock. [ESP-IDF example](../examples/esp-idf/peer/relay.c)

### Reproduce mixed-runtime qualification

Build the host targets with `cmake -S core -B build && cmake --build build`. Use a Unix MicroPython interpreter built with the project's user module. Run the driver from the project root.

```sh
MICROPYPATH=ports/micropython:.frozen python3 ports/micropython/tests/mixed_gatt.py \
    build/mm_gatt_test_node /path/to/micropython
```

The driver connects two independently restarted processes over a simulated 20-byte ATT characteristic. It exercises native origin → MicroPython relay → native destination and MicroPython origin → native relay → MicroPython destination. Each leg loses the first reply after a durable commit, retries, verifies the exact original frame and reclaims released data. The origin image is removed before B forwards. Physical pairing, RF reconnects, timing and board power cuts require device qualification. [Fault-injection driver](../ports/micropython/tests/mixed_gatt.py) · [Native gap and route tests](../core/tests/test_custody.c)

## First implementation milestone

The host integration test uses two nodes, one Pipe, a file-backed segment journal on each node, and a link that the test can interrupt. A saved stream and its batches identify the origin, Pipe key and generation. [Integration test](../core/tests/test_delivery.c) A destination receipt identifies its issuer, role, stream and accounted-for prefix. For custody, also bind the receipt to the intended final destination. A direct destination receipt already identifies it through the issuer. Test that changing a configured destination leaves the former destination’s receipts scoped to the former delivery. Use shared examples for the C and Rust codecs. The v0 HTTPS reply carries only the origin, Pipe key and sequence. [v0 batch and reply](../spec/wire-format.md#batch)

Use the following exchange as the acceptance test. A has committed records 1–3 locally. B is its named destination. Both begin with delivery progress at zero.

| Event | Original records durable on B | Prefix B can acknowledge | B’s prefix confirmed by A |
| --- | --- | --- | --- |
| B commits record 1. Drop its reply, then restart both nodes. | 1 | 1 | 0 |
| A retries record 1 and receives B’s receipt. | 1, exactly once | 1 | 1 |
| Deliver record 3 ahead of record 2 and return the receipt. | 1, 3 | 1 | 1 |
| Deliver record 2 and return the receipt. | 1, 2, 3 | 3 | 3 |

The v0 segment scanner stops at the first sequence gap. B therefore needs a place for record 3 while 2 is missing. The POSIX journal stores each new disjoint run in its own segment and recovers coverage from validated runs on reopen. Exercise this with a small storage budget. When B runs out of room for an incoming range, the sender must be able to retry it. [Segment scan](../spec/wire-format.md#segment)

Reopen both journals and verify B’s record bytes and origin against A’s original inputs. Then give A a fresh stream generation under the same Pipe key and deliver the old receipt to A. The new generation’s confirmed position must stay at zero. Once the host exchange passes, run the same scenario over BLE GATT between native ESP-IDF and MicroPython nodes. [Stream generations](adr/0006-identify-stream-generations.md) · [BLE interoperability](adr/0005-support-ble-custody-and-micropython.md)

Also exercise the opposite failure order. Make B’s storage sync fail before it can confirm a new record is durable. B must withhold an advanced receipt and A must keep its copy. An uncertain write may be visible after reopen, so retry the original bytes and verify that B counts the reading once. Group several records under one successful storage sync and one cumulative receipt. Measure storage syncs per committed batch, peak RAM and backlog drain rate on the target board. [Storage probe](architecture-evaluation.md#what-was-measured-on-flashdb)

## Composing a network

| Configuration | What success means |
| --- | --- |
| A → B | B’s destination receipt confirms the stream prefix B has durably accounted for. The exchange can complete entirely offline. |
| A → B → C | B can accept custody for onward delivery while C is unreachable. B’s custody receipt and C’s destination receipt identify their respective issuers and obligations. |
| A → B and B → A | Each node can collect and receive observations. Even with identical Pipe names, each origin’s generation, sequences and delivery cursors remain independent. |
| A → B → D and A → C → D | If the same readings reach D twice, D keeps one record per identity and preserves each distinct observed route for the ranges it carried. |

By default a delivery names one destination. A receiver acknowledges a contiguous accounted-for prefix, allowing one receipt to cover many records. If 101–110 arrive while 1–100 are outstanding, the receiver can store the later range immediately and advance its receipt when the earlier range is accounted for. Each sender tracks progress with the particular receiver. A sleeping peer elsewhere in the network can catch up when it reconnects. [Durable prefixes](adr/0004-acknowledge-a-durable-prefix-at-the-destination.md) · [Relay custody](adr/0005-support-ble-custody-and-micropython.md)

Whether an application also requires retained replicas, for how long, and how it replaces a lost copy are later deployment decisions. A delivery receipt records what its issuer accepted. A prefix that includes declared loss cannot serve as evidence of possession of the missing bytes.

## Preserving lineage

Suppose B forwards A’s records 1–100 to D and C forwards 51–120. D holds 120 original records. Its audit shows that 51–100 reached D by both paths. D can coalesce repeated transfers of the same range along the same path. Tracking coverage by range preserves that history when nodes split or combine batches.

A returning record in a cycle A → B → C → A still names A as its origin. A node that already has the record can record the additional route without scheduling another transfer solely because the route changed. Persist new route evidence independently of payload deduplication, and distinguish the immediate sender observed by a node from earlier hops reported by that sender.

A node publishing a correction or aggregate gives the derived record its own identity and links it to its source data. The delivery path answers how a record arrived. Input references answer how a different result was produced. The appropriate precision for those references remains an open design question. [Stream generations](adr/0006-identify-stream-generations.md) · [Glossary](../GLOSSARY.md)

Wire v0 already encodes one ordered path of up to eight hops per batch. Wire v1 adds generation-scoped receipts and preserves distinct paths across arrivals. Existing captures remain readable through the v0 decoder. [v0 batch format](../spec/wire-format.md#batch)
