# SPDX-License-Identifier: Apache-2.0
"""Run on MicroPython Unix or CPython with ports/micropython on the path."""
import asyncio
import struct
from mm_ble import GattSession, exchange

async def main():
    calls = []
    def receive(batch):
        calls.append(bytes(batch))
        return b"x" * 41
    state = GattSession(receive, batch_limit=80)
    async def write(fragment):
        assert len(fragment) <= 20
        state.write(fragment, 20)
    async def read():
        return state.read()
    batch = bytes(range(80))
    assert await exchange(write, read, batch, 20) == b"x" * 41
    assert calls == [batch]
    for fragment in (struct.pack("<BII", 1, 0, 81) + b"x",
                     struct.pack("<BII", 1, 7, 8) + b"x",
                     struct.pack("<BII", 2, 80, 20)):
        try:
            state.write(fragment, 20)
            assert False
        except ValueError:
            pass
        try:
            state.read()
            assert False
        except ValueError:
            pass
    assert calls == [batch]
asyncio.run(main())
