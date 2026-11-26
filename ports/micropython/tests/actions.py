# SPDX-License-Identifier: Apache-2.0
"""Drive the public dispatcher, prompt and serial byte stream on Unix MicroPython."""
import io
import json
import sys
import test_source
from micromeerschaum import ActionRegistry, Pipe, dispatch, make_action, provision, provision_peer
from mm_session import DeviceSession, FrameReader, PREFIX, serve_usb

root = sys.argv[1]
registry = ActionRegistry()
calls = []


@make_action("fixture ping")
def ping(args):
    return True, " ".join(args)


assert dispatch("fixture ping one two") == (True, "one two")
assert not registry.dispatch("fixture ping")[0]


@registry.make_action("field")
def field(args):
    calls.append(args)
    return True, "/".join(args)


@registry.make_action("field private")
def private(args):
    return True, "hidden"


@registry.make_action("field fail")
def fail(args):
    return False, "unavailable"


oversize_calls = []
@registry.make_action("long report")
def long_report(args):
    oversize_calls.append(True)
    return True, "x" * 4096


def provision_push():
    return provision("sensor", "room", root + "/actions.mm", 1, 21, 2, create=True)


p = provision_push()
source = provision("plugin:test_source", "room", root + "/source-actions.mm", 1, 22, 2,
                   location="north", create=True)
hidden = provision("sensor", "unpublished", root + "/unpublished.mm", 1, 23, 2, create=True)
test_source.fetch = lambda pipe: {"value": 41}
first_id = registry.expose(p, read=True, write=True)
second_id = registry.expose(source, read=True)
assert first_id < second_id
assert registry.dispatch("field 'abc def' e\\ f \"\"") == (True, "abc def/e f/")
assert registry.dispatch("field wrong\nfield wrong")[0] is False
assert not registry.dispatch('field "unfinished')[0]
assert registry.dispatch("field private", ("field",))[0] is False
assert len(calls) == 1
assert registry.dispatch("help", ("help", "field")) == (True, "field\nhelp")
assert registry.complete("show ") == ["show actions", "show pipes"]
assert registry.dispatch("sync pipes")[0] and source.get_docs() == [{"value": 41}]
assert p.get_docs() == []
ok, msg = registry.dispatch("sync pipes -m room")
assert not ok and "push-only" in msg and len(source.get_docs()) == 1
ok, msg = registry.dispatch("show pipes -c sensor -m room -l None -i journal:local")
assert ok and "sensor room" in msg and "plugin:" not in msg
sink = provision("sensor", "room", root + "/actions-sink.mm", 1, 21, 2,
                 receiver=True, instance="journal:sink", create=True)
provision_peer("peer:archive", 2, lambda batch: sink.receive(1, batch))

session = DeviceSession(registry, ("catalog", "read", "write", "actions", "action", "complete"),
                        ("field", "field fail", "help", "sync pipes"))
key = ["sensor", "room", None]


def frame(op, **kw):
    req = {"id": "123", "v": 1, "op": op}
    req.update(kw)
    return PREFIX + json.dumps(req).encode() + b"\n"


def call(op, **kw):
    writer = io.BytesIO()
    serve_usb(session, io.BytesIO(frame(op, **kw)), writer)
    reply = writer.getvalue()
    assert reply.startswith(PREFIX) and reply.endswith(b"\n") and len(reply) <= 4096, reply
    msg = json.loads(reply[len(PREFIX):])
    assert msg["v"] == 1 and msg["id"] == "123"
    return msg


oversized = DeviceSession(registry, ("action",), ("long report",))
reply = oversized.handle(frame("action", line="long report")[:-1])
assert not json.loads(reply[len(PREFIX):])["ok"] and len(oversize_calls) == 1
assert call("catalog", limit=1)["data"] == {
    "items": [{"id": first_id, "key": key, "instance": "journal:local", "read": True, "write": True,
               "filters": []}], "more": True}
assert call("catalog", offset=1)["data"]["items"][0]["id"] == second_id
assert not call("catalog", limit=65)["ok"]
assert call("action", line="help")["data"] == "field\nfield fail\nhelp\nsync pipes"
assert not call("action", line="field private")["ok"]
assert not call("action", line="field fail")["ok"]
assert not call("action", line="sync pipes -l north")["ok"]
assert call("complete", line="fi")["data"] == ["field", "field fail"]
assert "field private" not in call("actions")["data"]
row = {"tick": 9, "t": {"$us": -9}, "flag": True, "label": "north"}
assert call("write", key=key, rows=[row])["ok"]
assert p.progress()["high"] == 1 and p.progress()["confirmed"] == 0
assert call("read", key=key, limit=2)["data"] == [row]
assert call("read", key=key, limit=0)["data"] == []
assert not call("read", key=key, limit=129)["ok"]
assert not call("read", key=key, limit=1, params={"tick": 9})["ok"]
assert not call("read", key=key, limit=1, begin=0)["ok"]
assert not call("write", key=["plugin:test_source", "room", "north"], rows=[row])["ok"]
assert not call("write", key=["sensor", "unpublished", None], rows=[row])["ok"]
assert not call("write", key=["sensor", "new", None], rows=[row])["ok"]
assert not call("write", key=key, rows=[{"bad": {"$us": True}}])["ok"]
assert p.progress()["high"] == 1
good = frame("write", key=key, rows=[{"x": 89}])[:-1]
for bad in (good.replace(b',', b' ', 1), good[:-1] + b',}', good[:-1],
            good.replace(b'"write"', b'"write","op":"write"'), good + b"{}"):
    assert session.handle(bad) is None, bad
assert session.handle(PREFIX + b'{"id":"1","n":' + b"1" * 3900 + b"}") is not None
assert p.progress()["high"] == 1
line = frame("write", key=key, rows=[{"x": 1}])
received = []
framer = FrameReader()
for part in (b"garbage\n", b"@" * 4096, line, line[:-1]):
    framer.feed(part, lambda data: received.append(session.handle(data)))
assert received == [None] and p.progress()["high"] == 1
framer.feed(b"\n", lambda data: received.append(session.handle(data)))
assert p.progress()["high"] == 2 and len(received) == 2
# A lost response does not trigger a device-side retry.
assert call("read", key=key, limit=3)["data"] == [row, {"x": 1}]
assert registry.dispatch("copy pipes -m room -c sensor peer:archive")[0]
assert sink.get_docs()[1] == {"x": 1}
p.reclaim()
assert p.get_docs() == []
assert p.sync({"text": ["x" * 1400] * 3})[0]
assert not call("read", key=key, limit=3)["ok"]
assert call("read", key=key, limit=1)["ok"]
Pipe("sensor", "room").close()
assert not call("read", key=key, limit=1)["ok"]
p = provision_push()
assert not call("write", key=key, rows=[row])["ok"]
assert registry.expose(p, read=True, write=True) > second_id
reply = call("write", key=key, rows=[{"x": 1}] * 8 + [{"x": 1 << 63}])
assert not reply["ok"] and "confirmed 8 input rows" in reply["error"], reply
registry.unexpose(p)
registry.expose(p)
assert not call("write", key=key, rows=[row])["ok"]
screen = io.StringIO()
registry.shell(io.StringIO("field accidental\x03field hi\r\nsho?\nfield fai\t\nexit\n"), screen)
assert "show pipes" in screen.getvalue() and "unavailable" in screen.getvalue()
assert ["accidental"] not in calls and calls[-1] == ["hi"]
assert "^C\n[ journal:local ] > " in screen.getvalue()
assert not json.loads(DeviceSession(registry).handle(frame("actions")[:-1])[len(PREFIX):])["ok"]
for pipe in (p, source, sink, hidden):
    pipe.close()
print("public actions, prompt, live applications, stream framing, journal reads and writes")

# NimBLE reports temporary queue pressure as MP_EBUSY (16). Some builds
# omit that name from errno. Retry just the unqueued notification fragment.
import asyncio
from mm_action_ble import ManagementPeripheral


class BusyQueue:
    def __init__(self):
        self.tx = self
        self.fragments = []

    def authorized(self):
        return True

    def notify(self, conn, fragment):
        self.fragments.append(fragment)
        if len(self.fragments) == 1:
            raise OSError(16)


queue = BusyQueue()
assert asyncio.run(ManagementPeripheral._notify(queue, None, b"tail\n"))
assert queue.fragments == [b"tail\n", b"tail\n"]

# Bare @make_action, explicit help=, __doc__ fallback, and entry alias.
from micromeerschaum import default_actions, entry

bare_registry = ActionRegistry()


@bare_registry.make_action
def device_label(args):
    return True, "room monitor"


@bare_registry.make_action(help="  Report board status.  ")
def board_status(args):
    return True, "ok"


@bare_registry.make_action("sensor reset", help="Reset sensor state.")
def reset_sensor(args):
    return True, "reset"


class DocAction:
    __name__ = "doc_probe"
    __doc__ = "\n  Inspect diagnostic probe.\n  Second line ignored.\n"

    def __call__(self, args):
        return True, "probed"


doc_probe = bare_registry.make_action(DocAction())
assert bare_registry.dispatch("device label") == (True, "room monitor")
assert bare_registry.dispatch("board status") == (True, "ok")
assert bare_registry.dispatch("sensor reset") == (True, "reset")
assert bare_registry.dispatch("doc probe") == (True, "probed")
assert bare_registry.help_for("device label") is None
assert bare_registry.help_for("board status") == "Report board status."
assert bare_registry.help_for("sensor reset") == "Reset sensor state."
assert bare_registry.help_for("doc probe") == "Inspect diagnostic probe."
assert bare_registry.help_for("unknown") is None

# Existing help output format stays unchanged (action names only).
ok, help_out = bare_registry.dispatch("help")
assert ok and "device label" in help_out and "Report board status." not in help_out, help_out

# Reject invalid/duplicate bare and explicit registrations.
for bad in (
    lambda: bare_registry.make_action(lambda args: (True, "x")),
    lambda: bare_registry.make_action(device_label),
    lambda: bare_registry.make_action("bad  name"),
    lambda: bare_registry.make_action("valid name", help=123),
):
    try:
        bad()
        raise AssertionError("accepted invalid make_action call")
    except ValueError:
        pass


def _leading_underscore(args):
    return True, "no"


try:
    bare_registry.make_action(_leading_underscore)
    raise AssertionError("accepted leading underscore action name")
except ValueError:
    pass


def show_pipes(args):
    return True, "collision"


try:
    bare_registry.make_action(show_pipes)
    raise AssertionError("accepted built-in collision")
except ValueError:
    pass


@make_action
def module_bare_ping(args):
    return True, "bare:" + " ".join(args)


assert entry is dispatch
assert entry("module bare ping ok") == (True, "bare:ok")
assert default_actions.help_for("module bare ping") is None
print("bare @make_action, help metadata, docstring fallback, and entry alias")
