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
