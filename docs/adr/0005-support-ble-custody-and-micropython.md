# Support BLE custody transfer and MicroPython in the first release

The first independent release will include a generic BLE relay and MicroPython support alongside the portable C journal, HTTPS sender example and Meerschaum receiver plugin. An authorized durable relay may accept custody while offline once it has durably accounted for the stream prefix it acknowledges. The relay assumes responsibility for retaining and forwarding those records. Its receipt can release the origin from the forwarding obligation for that delivery. Final destination progress remains separately observable. [Peer destinations](0011-allow-peer-destinations.md)

A custody-capable relay needs persistent incoming records, replay after reboot, and an acknowledgement whose issuer and meaning the origin can verify. A volatile relay may forward the destination’s acknowledgement.

When a relay accepts custody, persist which final destination it agreed to reach along with the stream and prefix. Bind that destination to the receipt the sender verifies. For example, a relay’s promise to deliver to the greenhouse discharges only that delivery, even if the same relay serves a second archive. A direct destination receipt gets this scope from its issuer. [Receipt qualification](../peer-sync.md#first-implementation-milestone)

An authorized relay with insufficient space refuses new custody. Once it issues a custody receipt it protects that data until the next authorized custodian or the destination accepts responsibility. The first release will use fragmented GATT for interoperability between native ESP-IDF and ESP32 MicroPython and qualify both origin/relay role assignments.
