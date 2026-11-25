# MicroMeerschaum

MicroMeerschaum carries observations between intermittently connected devices and into Meerschaum. A deployment may keep its data entirely on peer devices.

## Language

**Node**

A participant in Pipe delivery. A node may originate records, receive them as a destination, or forward them for another origin.

**Device pipe**

An ordered stream of records from one origin, named by a connector, metric and optional location. Two origins can use the same name and have independent sequences.

**Origin**

The device that first recorded an observation. A record keeps its origin when another device forwards it.

**Derived record**

A new record produced from one or more source records, such as a site total or an operator’s correction. It has its own origin and identity and refers back to the records used to produce it.

**Sequence number**

A record’s position in a generation of its device pipe. Retries and relays preserve it so the receiver can recognize the same record.

**Stream generation**

One lifetime of a device pipe’s sequence numbering at an origin. A generation survives ordinary reboots and distinguishes records made before and after loss of the origin’s sequence state.

**Batch**

A transfer of consecutive records from one device pipe. An empty batch can ask the receiver for its cursor.

**Lineage**

The provenance of a record, including its origin, the observed routes by which copies reached a receiver, and the source records used in any derivation.

**Delivery route**

An observed sequence of transfers by which a receiver obtained a range of records. The same records can reach a receiver by several routes.

**Materialization**

The interpretation of received records as rows in an ordinary Meerschaum pipe. A deployment may derive several pipes from the same records.

**Materializer**

The component that maps a device pipe’s records to rows in one or more destination pipes. The built-in document materializer accepts named fields. A sensor-specific materializer interprets the sensor’s own payload.

**Device source plugin**

A named source of observations for one or more device pipes. It supplies documents in the application’s vocabulary.

**Device pipe sync**

A durable write to the Pipe’s selected target. The caller can supply a frame or use the Pipe’s source. A locally stored Pipe can sync while disconnected and track onward delivery separately.

**Instance connector**

The connector that identifies where a Pipe stores its data. A Pipe on the local node can have a separately configured delivery to a peer. A Pipe bound directly to that peer reports the outcome at the peer.

**Device instance**

A local target on the device that stores a pipe’s ordinary rows and supports reading them while offline. A row has the identity declared by its pipe and can reflect more than one journaled observation or correction.

**Outgoing record**

An immutable record retained by a sender for delivery. Updating a logical row can produce a new outgoing record even when the total number of rows stays the same.

**Retention policy**

A device pipe’s rule for which observations to preserve when it exhausts its storage. The rule applies to that pipe even when other pipes on the same device use a different policy.

**Replica**

A durable copy of original records on a particular node. A replica preserves the records’ identities and bytes. One holder can receive its copy over several routes.

**Delivery policy**

A pipe’s requirements for delivery, such as acceptance by a named destination or retention of copies on distinct authorized nodes. A sender uses the policy to decide which receipts satisfy its obligations.

**Loss record**

A record that describes observations the device missed or previously recorded data it discarded. Missed observations can have a count even when they have no sequence numbers.

**Durable acknowledgement**

A receiver’s assurance that it has durably accounted for every sequence through a stated position in one device pipe, including any explicitly declared losses. The issuer and its role determine which responsibility has been discharged.

**Upload cursor**

The position a sender has confirmed for one stream generation and delivery. It tracks who issued the receipt and which obligation it covers. An acknowledgement lost on the return trip can leave the sender’s cursor behind the receiver’s.

**Custody receipt**

An authorized relay’s durable acknowledgement that it has accepted responsibility for forwarding a stream prefix to a particular destination. It can release the previous custodian for that delivery while the final destination is offline.

**Destination receipt**

A designated destination’s durable acknowledgement that it has accounted for a stream prefix. A peer device can be a destination. Materialization can take place later.
