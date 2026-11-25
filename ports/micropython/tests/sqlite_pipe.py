# SPDX-License-Identifier: Apache-2.0
import sys
from micromeerschaum import Pipe, provision

path = sys.argv[1] + "/rows.db"
columns = (("tick", "int", False), ("station", "string", True), ("celsius", "float", False))

def open_rows(create=False):
    return provision("sensor", "temperature", path, 1, 7, 2, instance="sql:local",
                     columns=columns if create else None, axis="tick" if create else None,
                     create=create)

p = open_rows(True)
ok, message = p.sync({"tick": [1, 2], "station": ["A", "A"], "celsius": [20.0, 21.0]})
assert ok, message
assert p.sync({"tick": 1, "station": "A", "celsius": 22.0})[0]
assert p.sync({"tick": 2, "station": None, "celsius": 23.0})[0]
assert p.sync({"tick": 2, "station": None, "celsius": 24.0})[0]
p.close()
p = open_rows()
assert p.get_docs(begin=1, end=2, params={"station": ["A"]}, limit=3) == [
    {"tick": 1, "station": "A", "celsius": 22.0}]
assert len(p.get_docs()) == 3
assert p.get_docs(params={"station": None})[0]["celsius"] == 24.0
assert p.get_sync_time(params={"station": "A"}) == 2
assert p.get_sync_time(params={"station": "missing"}) is None
assert p.progress()["high"] == 5
dst = provision("sensor", "temperature", sys.argv[1] + "/sql-dst.mm", 1, 7, 2,
                instance="journal:sql-receiver", receiver=True, create=True)
while p.progress()["confirmed"] < p.progress()["high"]:
    p.confirm(2, dst.receive(1, p.batch()))
assert dst.read_doc(5)["celsius"] == 24.0
assert len(p.get_docs()) == 3
p.close()
dst.close()
print("persistent SQLite row corrections, filtered reads, delivery and replay")

origin = provision("sensor", "pressure", sys.argv[1] + "/custody.db", 10, 5, 30,
                   instance="sql:local", columns=(("tick", "int", True),), axis="tick",
                   custodian=20, create=True)
relay = provision("sensor", "pressure", sys.argv[1] + "/custody.mm", 10, 5, 30,
                  instance="journal:relay", custodian=20, receiver=True, create=True)
assert origin.sync({"tick": 3})[0]
original = origin.read_record(1)
packet = origin.batch()
receipt = relay.receive(10, packet)
assert packet == origin.batch()
origin.confirm(20, relay.receive(10, packet))
assert relay.read_record(1) == original
assert origin.progress()["confirmed"] == 1 and relay.progress()["confirmed"] == 0
assert origin.get_docs() == [{"tick": 3}]
relay.close()
origin.close()

wide = provision("sensor", "wide", sys.argv[1] + "/wide.db", 10, 1, 30,
                 instance="sql:local", segment_bytes=65536, create=True, axis="tick",
                 columns=(("tick", "int", True), ("left", "string", False),
                          ("right", "string", False)))
left = "a" * 33000
right = "b" * 33000
assert wide.sync({"tick": 1, "left": left})[0]
assert wide.sync({"tick": 1, "right": right})[0]
assert wide.get_docs(limit=1) == [{"tick": 1, "left": left, "right": right}]
wide.close()

# Host queries use the current SQLite rows, preserve typed timestamps, and
# release the cursor on size or validation failures.
import json
from micromeerschaum import ActionRegistry
from mm_session import DeviceSession, PREFIX

registry = ActionRegistry()
rows = provision("sensor", "events", sys.argv[1] + "/remote.db", 1, 9, 2, instance="sql:local",
                 columns=(("dt", "datetime", False), ("station", "string", True),
                          ("value", "int", False)), axis="dt", create=True)
registry.expose(rows, read=True, write=True)
session = DeviceSession(registry, ("catalog", "read", "write"))
key = ["sensor", "events", None]


def rpc(op, **kw):
    req = {"v": 1, "id": "db", "op": op}
    req.update(kw)
    response = session.handle(PREFIX + json.dumps(req).encode())
    return json.loads(response[len(PREFIX):])


assert rpc("catalog")["data"]["items"][0]["filters"] == ["begin", "end", "params"]
data = [{"dt": {"$us": us}, "station": "A", "value": us * 2} for us in range(4)]
assert rpc("write", key=key, rows=data)["ok"]
data[1]["value"] = 91
assert rpc("write", key=key, rows=[data[1]])["ok"]
assert rpc("read", key=key, limit=2, begin={"$us": 1}, end={"$us": 3},
           params={"station": ["A", "B"]})["data"] == data[1:3]
assert not rpc("read", key=key, limit=2, params={"bad": 1})["ok"]
rows.close()
print("remote SQLite catalog, typed time queries, keyed corrections and filter failures")
