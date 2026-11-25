# Acknowledge a durable prefix at the destination

An installable Meerschaum plugin will accept binary batches and retain raw records for replay before application-specific materialization. Its acknowledgement covers the highest sequence through which it has durably accounted for the entire prefix of that origin, device pipe and stream generation. Explicitly declared loss can account for a missing range. The sender may reclaim data when its delivery obligations permit it. A declared loss contributes no copy of the missing bytes. [Peer destinations](0011-allow-peer-destinations.md)

If records 101–110 arrive ahead of 1–100, the receiver can store 101–110 immediately. It advances the cursor through 110 when the missing range is accounted for. A parser outage can delay downstream rows without blocking ingestion.

[BLE custody transfer](0005-support-ble-custody-and-micropython.md) adds an earlier point where an authorized relay can take responsibility and release the origin. The destination cursor continues to describe final delivery.

Keep v0 archive decoding and v0 ingress on an explicit legacy path. Introduce generation-scoped receipts and explicit loss ranges in a separately versioned protocol. Qualify the first independent receiver on SQLite for a self-contained deployment and PostgreSQL for concurrent workers.

The ordinary Meerschaum row endpoint, a streaming source’s acknowledgement and a Pipeline edge cursor each have a different scope from this receipt. Use an authenticated plugin route for device batches. Qualify atomic raw capture, route evidence and prefix advancement with a token scoped to ingestion. A separate materializer can use the ordinary target Pipe’s permissions and replay after a failed or locked write. [Host integration and permission checks](../meerschaum-design.md#committing-a-receipt)
