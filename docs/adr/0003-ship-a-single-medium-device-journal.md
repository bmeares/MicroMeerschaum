# Ship a single-medium device journal

Applications need to recover committed observations and retry delivery after a restart. MicroMeerschaum will own a reusable journal that durably appends records, reopens and recovers one medium, and reads ranges by sequence. Device firmware owns sensor acquisition, RAM buffering, media selection, card replacement and power policy.

This puts one implementation of storage recovery within reach of other devices while keeping board-specific failure handling in the firmware that can observe it. The journal's storage engine is a separate decision.
