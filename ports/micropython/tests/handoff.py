# SPDX-License-Identifier: Apache-2.0
import sys
from micromeerschaum import provision

start = sys.argv[1] == "start"
src = provision("sensor", "handoff", "source.mm", 1, 7, 2, create=start)
dst = provision("sensor", "handoff", "receiver.mm", 1, 7, 2, create=start,
                receiver=True, instance="journal:receiver")

def deliver():
    src.confirm(2, dst.receive(1, src.batch()))

if start:
    assert src.sync({"tick": 1})[0]
    deliver()
    assert src.sync({"tick": 2})[0]
    with open("second.bin", "wb") as f:
        f.write(src.read_record(2))
else:
    assert src.progress()["confirmed"] == 1 and src.progress()["high"] == 2
    with open("second.bin", "rb") as f:
        assert f.read() == src.read_record(2)
    assert src.sync({"tick": 3})[0]
    deliver()
    assert src.progress()["confirmed"] == dst.progress()["prefix"] == 3
    for i in (1, 2, 3):
        assert src.read_record(i) == dst.read_record(i)
        assert dst.read_doc(i)["tick"] == i
src.close()
dst.close()
