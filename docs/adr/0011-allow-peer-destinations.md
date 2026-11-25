# Allow a device to be a final destination

A deployment can fulfill its storage goal entirely on microcontrollers. A designated peer can accept delivery in its own right, including while the whole site is offline. An upstream Meerschaum instance is optional.

Destination and custodian are roles relative to a delivery. A node may be the destination for a local archive and separately carry the same records upstream. Its local receipt establishes progress at that node. A separately configured upstream destination tracks its own progress.

Peers exchange immutable source records. Forwarding preserves the origin, device pipe key, generation, sequence and bytes. A peer publishing a correction or aggregate creates a record in its own stream and identifies its inputs. A destination that combines conflicting edits to the same logical row needs an application-defined merge policy.

The initial release supports delivery to a named destination. A deployment can compose several deliveries into a network. Future per-Pipe retention requirements may call for a named archive or copies on distinct authorized nodes. Preserve distinct observed delivery routes by record range even when the destination already holds the payload. Repeated deliveries over the same path and range may share an audit entry. [Peer sync scenarios](../peer-sync.md)
