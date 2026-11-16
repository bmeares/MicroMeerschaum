# Proposal: delegate device storage to an existing engine

Status: proposed. FlashDB is an evaluation candidate, not an adopted dependency.
This proposal does not change wire format v0 or its compatibility status.

## Goal

Minimize custom storage and recovery code maintained by MicroMeerschaum and its
device consumers. Keep MicroMeerschaum focused on portable records, cursor-range
sync, durable acknowledgement semantics, and relay lineage. Prefer an existing
storage engine when integration lets consumers delete substantial custom code.

## Dependency boundary

```text
Application observations
        |
MicroMeerschaum record encoding
        |
Device storage adapter -> storage engine (first candidate: FlashDB TSDB)
        |
MicroMeerschaum batches -> transport -> receiver durable acknowledgement
```

The portable C core and Rust receiver remain independent of the device storage
engine. A device-side adapter may depend on FlashDB. Store complete encoded
MicroMeerschaum records as opaque blobs, retaining their CRC, sequence, boot,
and monotonic timestamp. A storage-engine change should not require a new
upload protocol or a FlashDB dependency in the server decoder.

FlashDB could own allocation, record indexing, write-completion bookkeeping,
and rollover. The application still owns durable sequence identity, transport,
acknowledgement persistence, retention policy, and media lifecycle. FlashDB
does not itself implement MicroMeerschaum's end-to-end sync contract.

Do not introduce a backend registry or multiple production implementations for
this experiment. Start with one small adapter around existing APIs; decide its
public API and final repository location after measuring the integration.

## First experiment

Build an isolated host executable with pinned MicroMeerschaum and FlashDB
revisions and a recorded FlashDB configuration. Implement only:

1. Append a complete encoded record.
2. Read records starting at a sequence and form a valid MicroMeerschaum batch.
3. Close, reopen, recover, and append again.

Use FlashDB TSDB with variable-length blobs and 64-bit timestamps. Use sequence
as the strictly increasing TSDB timestamp, not UTC or boot-local time. Reject
sequences outside its signed timestamp range rather than narrowing silently.
Exercise sequence gaps as well as consecutive records; produce consecutive
ranges when constructing batches, as required by the existing wire format.

Validate returned records and constructed batches with the existing decoder and
shared vectors. Include small sensor payloads, variable-length payloads, loss
records, and clock-anchor documents. Refuse automatic formatting of an existing
damaged store. No firmware dependency or deployed storage migration is part of
the host experiment.

## Required behavior

| Concern | Acceptance condition |
|---|---|
| Durable append | After successful synchronization, reopening preserves committed records when the backend honors its sync contract |
| Interrupted append | The new record may be absent, but earlier committed records remain readable |
| Integrity | Validate the MicroMeerschaum CRC; completion status alone is insufficient |
| Sequence identity | Restore the next sequence from retained records and a persisted floor; lost or replaced media must not cause reuse |
| Replay | A lost acknowledgement permits retry with identical origin, sequence, and record bytes |
| Acknowledgement | Only a receiver's durable acknowledgement advances the upload cursor; a local TSDB status is not such an acknowledgement |
| Retention | Establish reclamation of acknowledged data and explicit reporting of forced loss of unacknowledged data |
| Corruption | Detect payload and metadata damage; record whether valid later records remain accessible and whether recovery preserves evidence |
| Full storage | Exercise rollover disabled and enabled; distinguish deliberate retention loss from write failure |

Inject failures during metadata, payload, completion-state, and rollover writes.
Include short writes, failed writes, and interrupted synchronization, then reopen
and verify the committed prefix and a subsequent append. Corruption tests should
also examine valid records after the damaged region, beyond prefix recovery.

The persisted floor and acknowledgement cursor are separate state: specify and
test their ordering with record commits. TSDB's last timestamp alone cannot
preserve identity after a medium is replaced or lost.

## Comparison and adoption gate

Use the same records and durability policy for the candidate and the consumer's
existing implementation. Report:

- Custom functions and tests removable, plus adapter and integration code added.
- Usable capacity, metadata overhead, and reserved space.
- Backend write, sync, and erase calls and bytes per committed record.
- Append, range-read, and recovery latency with an accumulated backlog.
- Failure-injection results and remaining application-owned failure paths.

Host file-mode results qualify integration logic only. Before production
adoption, repeat timing and physical power-cut tests on the intended raw flash
and filesystem-backed media. Account for acquisition latency, actual flash
wear, filesystem and card synchronization, and power consumption on hardware.
A storage dependency does not reduce an application's RAM buffering loss window.

For Walter, map the candidate against the existing journal writer and reader,
including SD failure handling, card swaps, and combined flash/SD reads. Those
remain required regardless of backend. The consumer's wiki owns its deployment
and hardware-specific contracts.

Adopt only if the candidate meets the failure contract and removes substantial
custom storage/recovery machinery. If it mostly adds an adapter while leaving
that machinery intact, retain the existing segment implementation and harden it.
Do not remove segment decoding or archive tools merely because device storage
changes; existing captures still need to be readable.

## Candidate references

- [FlashDB TSDB implementation](https://github.com/armink/FlashDB/blob/master/src/fdb_tsdb.c): append ordering, completion states, rollover, and initialization.
- [FlashDB configuration](https://github.com/armink/FlashDB/blob/master/docs/configuration.md): raw-flash and POSIX/libc file backends.
- [FlashDB definitions](https://github.com/armink/FlashDB/blob/master/inc/fdb_def.h): timestamp widths and control options.
- [MicroMeerschaum wire format](wire-format.md): records, segments, batches, and decoder contracts.

Upstream links explain the candidate. Pin an exact revision when implementing
the experiment; they do not establish that a particular integration is durable.
