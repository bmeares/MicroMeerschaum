# SPDX-License-Identifier: Apache-2.0
"""MicroPython Unix test node. stdin/stdout simulate ATT characteristic operations."""
import asyncio
import binascii
import sys
from micromeerschaum import provision
from mm_ble import GattSession, exchange


def emit(line):
    print(line)
    sys.stdout.flush()


role, path, op = sys.argv[1:]
pipe = provision("sensor", "temp", path, 1, 8, 9, custodian=None if role == "dest" else 2,
                 receiver=role != "origin", segments=4, segment_bytes=512, create=True)

async def send():
    async def write(value):
        emit("W" + binascii.hexlify(value).decode())
        if sys.stdin.readline().strip() != "OK":
            raise OSError("lost write")
    async def read():
        emit("R")
        value = sys.stdin.readline().strip()
        if not value.startswith("V"):
            raise OSError("lost reply")
        return binascii.unhexlify(value[1:])
    receipt = await exchange(write, read, pipe.batch(cap=512), 20)
    pipe.confirm(2 if role == "origin" else 9, receipt)
    pipe.reclaim()


if op == "seed":
    assert pipe.sync({"tick": 41})[0]
elif op == "status":
    state = pipe.progress()
    try:
        frame = binascii.hexlify(pipe.read_record(1)).decode()
    except OSError:
        frame = "-"
    emit("%d %d %s" % (state["prefix"], state["confirmed"], frame))
elif op == "send":
    try:
        asyncio.run(send())
    except OSError:
        pipe.close()
        sys.exit(3)
else:
    session = GattSession(lambda data: pipe.receive(1 if role == "relay" else 2, data), 512)
    while True:
        line = sys.stdin.readline().strip()
        if not line:
            break
        if line.startswith("W"):
            session.write(binascii.unhexlify(line[1:]), 20)
            emit("OK")
        elif line == "R":
            if op == "drop":
                emit("!")
                break
            emit("V" + binascii.hexlify(session.read()).decode())
        else:
            raise ValueError("unexpected ATT operation")
pipe.close()
