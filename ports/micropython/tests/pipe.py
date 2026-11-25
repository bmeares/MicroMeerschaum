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
pending = p.read_record(14)
assert remote.deliver()[0]
assert p.progress()["confirmed"] == 14 and dest.progress()["high"] == 14
assert dest.read_record(14) == pending
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

# Reopening a registered stream and constructing its API instance performs no exchanges.
p = origin()
dest = receiver()
provision_peer("api:cloud", 2, exchange)
before = len(requests)
api = Pipe("sensor", "temperature", instance="api:cloud")
assert len(requests) == before
assert api.sync({"tick": 101})[0] and len(requests) == before + 1
assert dest.read_doc(17)["tick"] == 101
p.close()
dest.close()
print("provisioned API instance and durable receipt")

a = provision("sensor", "relay", root + "/a.mm", 11, 6, 33, custodian=22, create=True)
b = provision("sensor", "relay", root + "/b.mm", 11, 6, 33, receiver=True,
              custodian=22, instance="journal:relay", create=True)
c = provision("sensor", "relay", root + "/c.mm", 11, 6, 33, receiver=True,
              instance="journal:final", create=True)
assert a.sync({"volts": 3.3})[0]
a.confirm(22, b.receive(11, a.batch()))
assert a.progress()["confirmed"] == 1 and b.progress()["confirmed"] == 0
assert b.routes() == [(1, 1, 11, ())]
provision_peer("peer:final", 33, lambda packet: c.receive(22, packet))
assert b.deliver("peer:final")[0]
assert c.routes() == [(1, 1, 22, (22,))]
assert c.read_record(1) == a.read_record(1)
assert b.progress()["confirmed"] == 1
a.close()
b.close()
c.close()
print("custody transfer and authenticated route evidence")

# Reuse a two-segment staging journal for direct writes and larger frames.
stage = provision("sensor", "staged", root + "/staging.mm", 1, 5, 2,
                  segments=2, create=True)
target = provision("sensor", "staged", root + "/target.mm", 1, 5, 2,
                   instance="journal:target", receiver=True, segments=64, create=True)
def accept(packet):
    return target.receive(1, packet)
for name in ("peer:near", "api:far"):
    provision_peer(name, 2, accept)
    direct = Pipe("sensor", "staged", instance=name)
    for tick in range(4):
        ok, message = direct.sync({"tick": tick})
        assert ok, message
    ok, message = direct.sync(({"tick": tick} for tick in range(24)))
    assert ok and direct.confirmed_input == direct.staged_input == 24, message
assert stage.progress()["confirmed"] == 56 and stage.progress()["missed"] == 0

def disconnected(packet):
    raise OSError("offline")
provision_peer("api:far", 2, disconnected)
ok, message = direct.sync(({"tick": tick} for tick in range(24)))
assert not ok and direct.staged_input == 16 and direct.confirmed_input == 0, message
assert stage.progress()["missed"] == 8
provision_peer("api:far", 2, accept)
assert direct.sync({"tick": 999})[0]  # Reconnect with a full pending journal.
assert target.read_doc(target.progress()["high"]) == {"tick": 999}
first = stage.progress()["high"] + 1
sent = []
def partial(packet):
    receipt = accept(packet)
    sent.append(1)
    if len(sent) == 2:
        raise OSError("lost reply")
    return receipt
provision_peer("api:far", 2, partial, max_records=1)
ok, message = direct.sync({"tick": [60, 61, 62]})
assert not ok and direct.confirmed_input == 1 and direct.staged_input == 3, message
assert stage.read_doc(first + 1) == target.read_doc(first + 1) == {"tick": 61}
provision_peer("api:far", 2, accept)
assert direct.deliver()[0]
assert target.read_doc(first + 2) == {"tick": 62}
stage.close()
target.close()
