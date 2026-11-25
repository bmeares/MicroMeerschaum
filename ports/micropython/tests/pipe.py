# Run with a fresh temporary directory as the first argument.
import sys
from micromeerschaum import Pipe, provision

root = sys.argv[1]
def origin(create=False):
    return provision("sensor", "temperature", root + "/source.mm", 1, 7, 2, create=create)

p = origin(True)
ok, message = p.sync(df={"tick": [41, 42], "celsius": [21.4, 21.7]})
assert ok, message
assert p.confirmed_input == 2
p.close()
p = origin()
assert Pipe("sensor", "temperature").progress()["high"] == 2
print("recovered two observations")

from micromeerschaum import Timestamp
assert p.read_doc(1) == {"tick": 41, "celsius": 21.4}
assert p.read_doc(2) == {"tick": 42, "celsius": 21.7}
assert p.sync({"tick": [1, 2], "celsius": [3]})[0] is False
assert p.progress()["high"] == 2
assert not p.sync()[0]
assert p.sync([])[0] and p.confirmed_input == 0
assert p.sync({"tick": []})[0] and p.confirmed_input == 0
assert p.sync({})[0] and p.confirmed_input == 0
assert p.sync([{}])[0] and p.confirmed_input == 1
scalars = {"lo": -(1 << 63), "hi": (1 << 63) - 1, "empty": "", "flag": False,
           "mark": Timestamp(-1), "missing": None, "name": "a\x00b"}
assert p.sync(scalars)[0]
saved = p.read_doc(4)
assert saved["mark"].us == -1
for name in ("lo", "hi", "empty", "flag", "missing", "name"):
    assert scalars[name] == saved[name]
for invalid in (1 << 63, -(1 << 63) - 1, float("inf"), {"nested": 1}, b"binary"):
    assert not p.sync([{"bad": invalid}])[0]
assert p.progress()["high"] == 4

def interrupted():
    for i in range(9):
        yield {"tick": i}
    yield {"bad": [1]}

assert not p.sync(interrupted())[0]
assert p.confirmed_input == 8
assert p.progress()["high"] == 12
assert p.read_doc(12) == {"tick": 7}
print("bounded row normalization and input prefix")

plugin = provision("plugin:test_source", "outside", root + "/weather.mm",
                   1, 9, 2, create=True)
assert plugin.register(unit="F")[0]
ok, message = plugin.sync(tick=3)
assert ok, message
assert plugin.read_doc(1) == {"tick": 3, "unit": "F"}
assert plugin.sync(df={"tick": 4, "unit": "direct"})[0]
assert plugin.read_doc(2)["unit"] == "direct"
fresh_handle = Pipe("plugin:test_source", "outside")
assert fresh_handle.sync(tick=5)[0]
assert fresh_handle.read_doc(3)["unit"] == "C"
plugin.close()
print("installed source registration, fetch, and supplied data")

from micromeerschaum import provision_peer
def receiver(create=False):
    return provision("sensor", "temperature", root + "/dest.mm", 1, 7, 2,
                     instance="journal:destination", receiver=True, create=create)

dest = receiver(True)
frame = p.batch(after=0, max_records=1)
reply = dest.receive(1, frame)
assert p.progress()["confirmed"] == 0
dest.close()
p.close()
dest = receiver()
p = origin()
assert dest.receive(1, frame) == reply
assert dest.read_record(1) == p.read_record(1)
try:
    p.confirm(3, reply)
    raise AssertionError("accepted unauthenticated receipt")
except OSError:
    pass
p.confirm(2, reply)
assert p.progress()["confirmed"] == 1

requests = []
def exchange(frame):
    requests.append(1)
    return dest.receive(1, frame)

provision_peer("peer:greenhouse", 2, exchange)
assert p.copy_to("peer:greenhouse")[0]
assert p.progress()["confirmed"] == 12
assert dest.progress()["prefix"] == 12
before = len(requests)
remote = Pipe("sensor", "temperature", instance="peer:greenhouse")
assert remote.metric_key == "temperature" and len(requests) == before
ok, message = remote.sync({"tick": 50})
assert ok, message
assert len(requests) == before + 1 and dest.read_doc(13)["tick"] == 50

def lost(frame):
    dest.receive(1, frame)
    raise OSError("lost reply")

provision_peer("peer:greenhouse", 2, lost)
ok, message = remote.sync({"tick": 51})
assert not ok and remote.staged_input == 1 and remote.confirmed_input == 0, message
assert p.progress()["confirmed"] == 13 and dest.progress()["prefix"] == 14
p.close()
dest.close()
p = origin()
dest = receiver()
provision_peer("peer:greenhouse", 2, exchange)
assert remote.deliver()[0]
assert p.progress()["confirmed"] == 14 and dest.progress()["high"] == 14
assert dest.read_record(14) == p.read_record(14)
p.reclaim()
assert p.progress()["high"] == 14
print("provisioned peer copy, direct sync, lost reply and reboot")

class Frame:
    columns = ("tick", "celsius")
    def itertuples(self, index, name):
        assert not index and name is None
        yield (90, 12.5)
        yield (91, 13.0)

assert p.sync(Frame())[0] and p.confirmed_input == 2
assert p.read_doc(16) == {"tick": 91, "celsius": 13.0}

small = provision("sensor", "small", root + "/small.mm", 1, 8, 2,
                  segments=2, segment_bytes=512, create=True)
ok, message = small.sync(({"tick": i} for i in range(24)))
assert not ok and small.confirmed_input == 16, message
assert small.progress()["missed"] == 8
small.close()
p.close()
dest.close()
print("frame adapter and durable overflow count")
