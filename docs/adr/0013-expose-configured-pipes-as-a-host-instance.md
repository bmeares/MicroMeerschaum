# Expose configured device Pipes as a host instance

A Meerschaum host selects a node with an instance key such as `micro:bench`. Its connector enumerates the Pipes the running application publishes, reads bounded selections from readable document Pipes, and syncs supplied rows into explicitly writable Pipes. A source on that node keeps its own connector, metric and location. Ordinary Meerschaum applications can move documents between instances using Pipe APIs.

The same device session can run explicitly authorized actions over USB serial or authenticated, bonded BLE. A caller that loses the reply to a submitted write or action reports an unknown outcome. A retry could append another reading or repeat a hardware operation. Journal batch delivery retains its existing receipt protocol. Brokered action delivery would need its own expiration and redelivery rules.
