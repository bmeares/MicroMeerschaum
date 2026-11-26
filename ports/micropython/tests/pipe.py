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
assert p.columns == {}
assert p.dtypes == {}
assert p.get_columns_types() == {}
assert p.get_rowcount() == 2
assert p.get_rowcount(params={}) == 2
assert p.get_docs(limit=2, order="asc") == [{"tick": 41, "celsius": 21.4}, {"tick": 42, "celsius": 21.7}]
for bad_kw in ({"begin": 1}, {"end": 2}, {"params": {"tick": 41}}):
    try:
        p.get_rowcount(**bad_kw)
        raise AssertionError("accepted journal rowcount filter")
    except ValueError:
        pass
for bad_order in ("desc", "invalid"):
    try:
        p.get_docs(order=bad_order)
        raise AssertionError("accepted unsupported journal order")
    except ValueError:
        pass
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
assert p.get_rowcount() == 0
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
assert small.get_rowcount() == 16
small.close()
keep_new = provision("sensor", "rolling", root + "/rolling.mm", 1, 8, 2,
                     segments=2, segment_bytes=512, retention="keep-new", create=True)
assert keep_new.sync(({"tick": i} for i in range(24)))[0]
assert keep_new.get_rowcount() == len(keep_new.get_docs(limit=100)) < 24
assert keep_new.get_rowcount() > 0
keep_new.close()
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

# Pipe identity, repr/str, equality/hashing, constructor aliases, helpers, grammar, and get_pipes.
import micromeerschaum as mrsm
from micromeerschaum import get_pipes

assert mrsm.__version__ == "0.2.1"
assert mrsm.entry is mrsm.dispatch

p_default = Pipe("sensor", "temperature")
p_loc = Pipe("sensor", "temperature", "east")
p_none_str = Pipe("sensor", "temperature", "None")
p_empty_str = Pipe("sensor", "temperature", "")
p_sql = Pipe("sensor", "temperature", instance="sql:local")
p_loc_sql = Pipe("sensor", "temperature", "east", "sql:local")
p_explicit_local = Pipe("sensor", "temperature", "east", "journal:local")

assert repr(p_default) == str(p_default) == "Pipe('sensor', 'temperature')"
assert repr(p_loc) == str(p_loc) == "Pipe('sensor', 'temperature', 'east')"
assert repr(p_none_str) == str(p_none_str) == "Pipe('sensor', 'temperature', 'None')"
assert repr(p_empty_str) == str(p_empty_str) == "Pipe('sensor', 'temperature', '')"
assert repr(p_sql) == str(p_sql) == "Pipe('sensor', 'temperature', instance='sql:local')"
assert repr(p_loc_sql) == str(p_loc_sql) == "Pipe('sensor', 'temperature', 'east', instance='sql:local')"
assert repr(p_explicit_local) == "Pipe('sensor', 'temperature', 'east')"

# Equality and hashing across exact identity tuples.
p_alias = Pipe(connector_keys="sensor", metric_key="temperature",
               location_key="east", mrsm_instance="sql:local")
p_alias2 = Pipe(connector_keys="sensor", metric_key="temperature",
                location_key="east", instance_keys="sql:local")
assert p_loc_sql == p_alias == p_alias2
assert hash(p_loc_sql) == hash(p_alias) == hash(p_alias2)
assert len({p_loc_sql, p_alias, p_alias2, p_default, p_loc, p_none_str, p_empty_str, p_sql}) == 6
lookup = {p_loc_sql: "hit"}
assert lookup[p_alias] == "hit"
assert p_default != p_none_str and p_default != p_empty_str and p_default != p_sql
assert p_default != ("sensor", "temperature", None, "journal:local")
assert Pipe(connector_keys="sensor", metric_key="temperature", location_key="None").location_key == "None"
assert Pipe("sensor", "temperature", connector_keys="sensor", metric_key="temperature") == p_default

for bad_kwargs in (
    {},
    {"connector": "sensor"},
    {"metric": "temperature"},
    {"connector": "sensor", "metric": "temperature", "connector_keys": "other"},
    {"connector": "sensor", "metric": "temperature", "metric_key": "other"},
    {"connector": "sensor", "metric": "temperature", "location": "a", "location_key": "b"},
    {"connector": "sensor", "metric": "temperature", "instance": "sql:local", "mrsm_instance": "journal:local"},
    {"connector": "sensor", "metric": "temperature", "instance": "sql:local", "instance_keys": "journal:other"},
    {"connector": "sensor", "metric": "temperature", "instance": ""},
):
    try:
        Pipe(**bad_kwargs)
        raise AssertionError("accepted invalid Pipe constructor kwargs: %r" % (bad_kwargs,))
    except ValueError:
        pass

# exists(), get_doc(), show(), fetch(), and singular/plural sync messages.
unprov = Pipe("sensor", "parity")
assert not unprov.exists()
captured = []
orig_print = getattr(mrsm, "print", None)
mrsm.print = lambda *a: captured.append(" ".join(str(x) for x in a))
try:
    assert unprov.show() == (True, "Success")
finally:
    if orig_print is None:
        del mrsm.print
    else:
        mrsm.print = orig_print
assert "Pipe('sensor', 'parity')" in captured[-1]
assert "instance: journal:local" in captured[-1]
assert "exists: False" in captured[-1]

parity = provision("sensor", "parity", root + "/parity.mm", 1, 31, 2,
                   segments=4, segment_bytes=512, create=True)
assert parity.exists() and Pipe("sensor", "parity").exists()
assert parity.get_doc() is None
ok, message = parity.sync([])
assert ok and message == "committed 0 input rows", message
ok, message = parity.sync({"tick": 1})
assert ok and message == "committed 1 input row", message
ok, message = parity.sync({"tick": [2, 3]})
assert ok and message == "committed 2 input rows", message
assert parity.get_doc() == {"tick": 1}

# Partial failure with exactly 1 confirmed input row uses singular grammar.
def one_then_bad():
    yield {"pad": "x" * 220}
    yield {"pad": "y" * 220}
    yield {"bad": [1]}

ok, message = parity.sync(one_then_bad())
assert not ok and parity.confirmed_input == 1 and "confirmed 1 input row)" in message, message

captured = []
mrsm.print = lambda *a: captured.append(" ".join(str(x) for x in a))
try:
    assert parity.show() == (True, "Success")
finally:
    del mrsm.print
assert "Pipe('sensor', 'parity')" in captured[-1]
assert "exists: True" in captured[-1]
assert "progress:" in captured[-1] and "high=4" in captured[-1]

# fetch() runs source registration (if needed) and fetch without committing rows.
src_pipe = provision("plugin:test_source", "fetch_only", root + "/fetch_only.mm", 1, 32, 2, create=True)
doc = src_pipe.fetch(tick=10)
assert doc == {"tick": 10, "unit": "C"} and src_pipe.parameters == {"unit": "C"}
assert src_pipe.progress()["high"] == 0 and src_pipe.get_doc() is None
assert src_pipe.register(unit="K")[0]
assert src_pipe.fetch(tick=11) == {"tick": 11, "unit": "K"}
assert src_pipe.progress()["high"] == 0
ok, message = src_pipe.sync(tick=12)
assert ok and message == "committed 1 input row" and src_pipe.get_doc() == {"tick": 12, "unit": "K"}
try:
    parity.fetch()
    raise AssertionError("fetch succeeded on non-plugin Pipe")
except ValueError:
    pass

# get_pipes() nested dict, as_list, exact keys, and scalar/list/negation/None filters.
loc_east = provision("sensor", "humidity", root + "/hum_east.mm", 1, 33, 2, location="east", create=True)
loc_none_str = provision("sensor", "parity", root + "/parity_none_str.mm", 1, 34, 2, location="None", create=True)
other_inst = provision("sensor", "humidity", root + "/hum_rx.mm", 1, 33, 2,
                       location="east", instance="journal:replica", receiver=True, create=True)

nested = get_pipes(instance_keys="journal:local")
assert nested["sensor"]["parity"][None] == parity
assert nested["sensor"]["parity"]["None"] == loc_none_str
assert nested["sensor"]["humidity"]["east"] == loc_east
assert nested["plugin:test_source"]["fetch_only"][None] == src_pipe

all_list = get_pipes(as_list=True)
assert len(all_list) == 5
assert set(all_list) == {parity, src_pipe, loc_east, loc_none_str, other_inst}
assert set(get_pipes(connector_keys="sensor", as_list=True)) == {parity, loc_east, loc_none_str, other_inst}
assert set(get_pipes(connector_keys=["plugin:test_source"], as_list=True)) == {src_pipe}
assert set(get_pipes(connector_keys="_sensor", as_list=True)) == {src_pipe}
assert set(get_pipes(metric_keys=("parity", "fetch_only"), as_list=True)) == {parity, loc_none_str, src_pipe}
assert set(get_pipes(metric_keys="_parity", as_list=True)) == {src_pipe, loc_east, other_inst}
assert set(get_pipes(location_keys="east", as_list=True)) == {loc_east, other_inst}
assert set(get_pipes(location_keys=[None], as_list=True)) == {parity, src_pipe}
assert set(get_pipes(location_keys="[None]", as_list=True)) == {parity, src_pipe}
assert set(get_pipes(location_keys="None", as_list=True)) == {parity, src_pipe, loc_none_str}
assert set(get_pipes(location_keys="_None", as_list=True)) == {loc_east, other_inst}
assert set(get_pipes(location_keys="_[None]", as_list=True)) == {loc_east, loc_none_str, other_inst}
assert set(get_pipes(location_keys=["east", "_east"], as_list=True)) == set()
assert set(get_pipes(instance_keys="journal:replica", as_list=True)) == {other_inst}
assert set(get_pipes(instance_keys="_journal:replica", as_list=True)) == {parity, src_pipe, loc_east, loc_none_str}
assert get_pipes(mrsm_instance="journal:replica", as_list=True) == [other_inst]

for handle in (parity, src_pipe, loc_east, loc_none_str, other_inst):
    handle.close()
assert not parity.exists()
assert get_pipes() == {} and get_pipes(as_list=True) == []
print("Pipe parity: repr/eq/hash, aliases, exists/fetch/get_doc/show, grammar, get_pipes")
