# Give a device pipe a new generation after loss of identity state

A record is identified by origin, device pipe key, stream generation and sequence number. Normal reboots, retries and card swaps keep the generation. A factory reset that loses the sequence authority starts a fresh generation before producing new records, even when the physical origin ID and pipe name survive.

An offline device can then resume collection after a reset, and an old card can return with records from the previous generation. Journals, receipts, relay spools and receiver uniqueness constraints must carry the generation. The generation identifier must be issuable without silently reusing an old value.
