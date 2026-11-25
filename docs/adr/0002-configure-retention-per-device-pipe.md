# Configure retention per device pipe

By default a full device pipe protects its previously journaled, unacknowledged records and reports observations missed while full. A deployment may explicitly choose to keep new observations and report the sequences it evicts. Either policy must preserve enough evidence to report loss durably when space becomes available, even when the data area is full.

The choice matters for disconnected nodes. An archive may value the earliest surviving observations, while a live monitoring application may value the most recent window. Storage reclamation must respect the policy selected for the pipe.
