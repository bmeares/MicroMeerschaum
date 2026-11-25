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
