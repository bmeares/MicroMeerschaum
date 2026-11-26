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
assert p.columns == {"datetime": "tick", "station": "station"}
assert p.dtypes == {"tick": "int", "station": "string", "celsius": "float"}
assert p.get_columns_types() == {"tick": "int", "station": "string", "celsius": "float"}
assert p._config().store.definition() == (
    "tick",
    (("tick", "int", False), ("station", "string", True), ("celsius", "float", False)),
    False,
)
assert p.get_rowcount() == 3
assert p.get_rowcount(begin=1, end=2) == 1
assert p.get_rowcount(begin=1, end=3, params={"station": ["A"]}) == 2
assert p.get_rowcount(params={"station": None}) == 1
assert p.get_rowcount(params={"station": []}) == 0
assert p.get_rowcount(begin=5, end=2) == 0
for bad_kw in ({"params": {"unknown": 1}}, {"begin": "bad"}, {"params": {"station": 123}}):
    try:
        p.get_rowcount(**bad_kw)
        raise AssertionError("accepted invalid rowcount filter")
    except (ValueError, TypeError):
        pass
assert p.get_docs(begin=1, end=2, params={"station": ["A"]}, limit=3) == [
    {"tick": 1, "station": "A", "celsius": 22.0}]
assert p.get_doc(begin=1, end=2, params={"station": ["A"]}) == {
    "tick": 1, "station": "A", "celsius": 22.0}
assert p.get_doc(params={"station": "missing"}) is None
assert p.get_docs(order="asc") == [
    {"tick": 1, "station": "A", "celsius": 22.0},
    {"tick": 2, "station": "A", "celsius": 21.0},
    {"tick": 2, "station": None, "celsius": 24.0},
]
assert p.get_docs(order="desc") == [
    {"tick": 2, "station": None, "celsius": 24.0},
    {"tick": 2, "station": "A", "celsius": 21.0},
    {"tick": 1, "station": "A", "celsius": 22.0},
]
assert p.get_docs(order="desc", limit=2) == [
    {"tick": 2, "station": None, "celsius": 24.0},
    {"tick": 2, "station": "A", "celsius": 21.0},
]
assert p.get_docs(begin=1, end=3, params={"station": "A"}, order="desc") == [
    {"tick": 2, "station": "A", "celsius": 21.0},
    {"tick": 1, "station": "A", "celsius": 22.0},
]
assert p._config().store.docs(None, None, None, 2, None, True) == [
    {"tick": 2, "station": None, "celsius": 24.0},
    {"tick": 2, "station": "A", "celsius": 21.0},
]
assert p._config().store.docs(None, None, None, 2, 4096) == [
    {"tick": 1, "station": "A", "celsius": 22.0},
    {"tick": 2, "station": "A", "celsius": 21.0},
]
try:
    p.get_docs(order="sideways")
    raise AssertionError("accepted invalid order")
except ValueError:
    pass
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
assert origin.columns == {"datetime": "tick"}
assert origin.dtypes == {"tick": "int"}
assert origin.get_docs() == [{"tick": 3}]
relay.close()
origin.close()

# Standard Meerschaum dict provisioning (columns + dtypes) on create and reopen.
from collections import OrderedDict
dict_path = sys.argv[1] + "/dict.db"
dict_cols = {"datetime": "tick", "id": "station", "ignored": None}
dict_dtypes = {"tick": "int", "station": "string", "celsius": "float", "ok": "bool"}
dp = provision("sensor", "dict_temp", dict_path, 1, 11, 2, instance="sql:local",
               columns=dict_cols, dtypes=dict_dtypes, create=True)
assert dp.columns == {"datetime": "tick", "station": "station"}
assert dp.dtypes == dict_dtypes
assert dp.get_columns_types() == dict_dtypes
assert dp._config().store.definition()[0] == "tick"
assert dp.sync({"tick": [10, 20], "station": ["S1", "S2"], "celsius": [19.5, 20.5], "ok": [True, False]})[0]
assert dp.sync({"tick": 10, "station": "S1", "celsius": 21.5, "ok": True})[0]
assert dp.get_rowcount() == 2
assert dp.get_docs(order="desc") == [
    {"tick": 20, "station": "S2", "celsius": 20.5, "ok": False},
    {"tick": 10, "station": "S1", "celsius": 21.5, "ok": True},
]
dp.close()

# Reopen without schema restores stored columns and dtypes.
dp = provision("sensor", "dict_temp", dict_path, 1, 11, 2, instance="sql:local", create=False)
assert dp.columns == {"datetime": "tick", "station": "station"}
assert dp.dtypes == dict_dtypes
assert dp.get_rowcount() == 2
dp.close()

# Reopen with matching dict schema (including matching explicit axis and different key order) succeeds.
reordered_dtypes = {"ok": "bool", "celsius": "float", "station": "string", "tick": "int"}
dp = provision("sensor", "dict_temp", dict_path, 1, 11, 2, instance="sql:local",
               columns={"datetime": "tick", "id": "station"}, axis="tick",
               dtypes=reordered_dtypes, create=False)
assert dp.get_rowcount(params={"station": "S1"}) == 1
dp.close()

# OrderedDict preserves explicit column order and detects order mismatches on reopen.
od_path = sys.argv[1] + "/ordered.db"
od_dtypes = OrderedDict([("tick", "int"), ("station", "string"), ("celsius", "float")])
odp = provision("sensor", "od_temp", od_path, 1, 11, 2, instance="sql:local",
                columns={"datetime": "tick", "id": "station"}, dtypes=od_dtypes, create=True)
assert odp._config().store.definition() == (
    "tick",
    (("tick", "int", False), ("station", "string", True), ("celsius", "float", False)),
    False,
)
odp.close()
odp = provision("sensor", "od_temp", od_path, 1, 11, 2, instance="sql:local",
                columns={"datetime": "tick", "id": "station"}, dtypes=od_dtypes, create=False)
odp.close()
try:
    provision("sensor", "od_temp", od_path, 1, 11, 2, instance="sql:local",
              columns={"datetime": "tick", "id": "station"},
              dtypes=OrderedDict([("station", "string"), ("tick", "int"), ("celsius", "float")]),
              create=False)
    raise AssertionError("accepted reordered OrderedDict schema on reopen")
except OSError:
    pass

# Reopen with mismatched schema fails cleanly with conflict.
for bad_cols, bad_dtypes in (
    ({"datetime": "tick"}, dict_dtypes),  # missing extra key
    (dict_cols, {"tick": "int", "station": "string", "celsius": "int", "ok": "bool"}),  # wrong dtype
    (dict_cols, {"tick": "int", "station": "string", "celsius": "float"}),  # missing column
):
    try:
        provision("sensor", "dict_temp", dict_path, 1, 11, 2, instance="sql:local",
                  columns=bad_cols, dtypes=bad_dtypes, create=False)
        raise AssertionError("accepted mismatched schema on reopen")
    except OSError:
        pass

# Reopen of nonexistent DB with schema does not create a file.
try:
    provision("sensor", "missing_db", sys.argv[1] + "/nope.db", 1, 11, 2, instance="sql:local",
              columns={"datetime": "tick"}, dtypes={"tick": "int"}, create=False)
    raise AssertionError("created DB when create=False")
except OSError:
    pass

# Dict provisioning with datetime axis + autotime.
dt_path = sys.argv[1] + "/dict_dt.db"
dtp = provision("sensor", "dict_dt", dt_path, 1, 12, 2, instance="sql:local",
                columns={"datetime": "ts"}, dtypes={"ts": "datetime", "v": "int"},
                autotime=True, create=True)
assert dtp.columns == {"datetime": "ts"}
assert dtp.dtypes == {"ts": "datetime", "v": "int"}
assert dtp._config().store.definition()[2] is True
assert dtp.sync({"v": 7})[0]
assert dtp.get_rowcount() == 1
dtp.close()
try:
    provision("sensor", "dict_dt", dt_path, 1, 12, 2, instance="sql:local",
              columns={"datetime": "ts"}, dtypes={"ts": "datetime", "v": "int"},
              autotime=False, create=False)
    raise AssertionError("accepted autotime mismatch on reopen")
except OSError:
    pass

# Validation errors for dict/tuple mixing and invalid declarations.
for bad_kw in (
    {"columns": (("tick", "int", False),), "dtypes": {"tick": "int"}, "axis": "tick"},
    {"columns": {"datetime": "tick"}},
    {"columns": {"id": "station"}, "dtypes": {"station": "string"}},
    {"columns": {"datetime": "tick"}, "axis": "other", "dtypes": {"tick": "int"}},
    {"columns": {"datetime": "tick", "id": "station"}, "dtypes": {"tick": "int"}},
    {"columns": {"datetime": "tick"}, "dtypes": {"tick": "string"}},
    {"columns": {"datetime": "tick"}, "dtypes": {"tick": "int", "v": "decimal"}},
    {"columns": {"datetime": "tick"}, "dtypes": {"tick": "int"}, "autotime": True},
    {"columns": {"datetime": "tick", "id": "tick"}, "dtypes": {"tick": "int"}},
):
    try:
        provision("sensor", "bad", sys.argv[1] + "/bad.db", 1, 13, 2, instance="sql:local",
                  create=True, **bad_kw)
        raise AssertionError("accepted invalid schema: %r" % (bad_kw,))
    except ValueError:
        pass

try:
    provision("sensor", "bad_journal", sys.argv[1] + "/bad_journal.mm", 1, 13, 2,
              dtypes={"tick": "int"}, create=True)
    raise AssertionError("accepted dtypes on journal")
except ValueError:
    pass
print("SQLite schema inspection, dict provisioning, rowcount, and descending reads")

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
