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
assert registry.dispatch("help", ("help", "field")) == (True, "field\nhelp  help [ACTION]")
assert registry.complete("show ") == [
    "show actions",
    "show columns",
    "show connectors",
    "show data",
    "show pipes",
    "show progress",
    "show rowcounts",
    "show version",
]
ok, msg = registry.dispatch("sync pipes")
assert ok and msg == "+ Pipe('plugin:test_source', 'room', 'north')  committed 1 input row"
assert source.get_docs() == [{"value": 41}]
assert p.get_docs() == []
ok, msg = registry.dispatch("sync pipes -m room")
assert not ok and "push-only" in msg and len(source.get_docs()) == 1
ok, msg = registry.dispatch("show pipes -c sensor -m room -l None -i journal:local")
assert ok and msg == "Pipe('sensor', 'room')" and "plugin:" not in msg
ok, msg = registry.dispatch("show pipe -l [None]")
assert ok and msg == "Pipe('sensor', 'room')"
ok, msg = registry.dispatch("show pipes -l _[None]")
assert ok and msg == "Pipe('plugin:test_source', 'room', 'north')"
ok, msg = registry.dispatch("show pipes -l _None")
assert ok and msg == "Pipe('plugin:test_source', 'room', 'north')"
ok, msg = registry.dispatch("show pipes -c _sensor")
assert ok and msg == "Pipe('plugin:test_source', 'room', 'north')"
ok, msg = registry.dispatch("show pipes -c sensor plugin:test_source _sensor")
assert ok and msg == "Pipe('plugin:test_source', 'room', 'north')"
ok, msg = registry.dispatch("show pipes -c sensor -c plugin:test_source")
assert ok and msg == "Pipe('sensor', 'room')\nPipe('plugin:test_source', 'room', 'north')"
ok, msg = registry.dispatch("sync pipe -c _sensor")
assert ok and msg == "+ Pipe('plugin:test_source', 'room', 'north')  committed 1 input row"
assert len(source.get_docs()) == 2
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
assert call("action", line="help")["data"] == (
    "field\n"
    "field fail\n"
    "help  help [ACTION]\n"
    "sync pipes (sync pipe)  Fetch and commit rows from matching source Pipes."
)
assert call("action", line="help sync pipe")["data"] == (
    "sync pipes (sync pipe)  Fetch and commit rows from matching source Pipes."
)
assert not call("action", line="help field private")["ok"]
assert not call("action", line="help show pipes")["ok"]
assert not call("action", line="help show pipe")["ok"]
assert not call("action", line="field private")["ok"]
assert not call("action", line="field fail")["ok"]
assert not call("action", line="sync pipes -l north")["ok"]
assert not call("action", line="sync pipe -l north")["ok"]
assert call("complete", line="fi")["data"] == ["field", "field fail"]
assert "field private" not in call("actions")["data"]
assert "sync pipe" not in call("actions")["data"]
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
ok, msg = registry.dispatch("copy pipes -m room -c sensor peer:archive")
assert ok and msg.startswith("+ Pipe('sensor', 'room')  "), msg
assert sink.get_docs()[1] == {"x": 1}
ok, msg = registry.dispatch("copy pipe -I peer:archive -c sensor -m room")
assert ok and msg.startswith("+ Pipe('sensor', 'room')  "), msg
ok, msg = registry.dispatch("copy pipes --target-instance peer:archive -c sensor -l [None]")
assert ok and msg.startswith("+ Pipe('sensor', 'room')  "), msg
for bad_copy in (
    "copy pipes peer:archive -I peer:archive",
    "copy pipes -I peer:archive peer:archive",
    "copy pipes -I peer:archive --target-instance peer:archive",
    "copy pipes -I",
    "copy pipes -I notpeer",
    "show pipes -I peer:archive",
    "sync pipes -I peer:archive",
):
    assert not registry.dispatch(bad_copy)[0], bad_copy
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
shell_out = screen.getvalue()
assert shell_out.startswith("MicroMeerschaum 0.2.1\n[ journal:local ] > "), shell_out
assert "+ hi\n" in shell_out and "+ show pipes\n" in shell_out and "- unavailable\n" in shell_out, shell_out
assert ["accidental"] not in calls and calls[-1] == ["hi"]
assert "^C\n[ journal:local ] > " in shell_out
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

# Help and show actions include descriptions and aliases, and respect remote gating.
ok, help_out = bare_registry.dispatch("help")
assert ok and "device label" in help_out and "board status  Report board status." in help_out, help_out
assert "show actions (show action)  List available actions." in help_out, help_out
assert "show pipes (show pipe)  List matching configured Pipes." in help_out, help_out
assert bare_registry.dispatch("show actions") == (True, help_out)
assert bare_registry.dispatch("show action") == (True, help_out)
assert not bare_registry.dispatch("show actions extra")[0]
assert bare_registry.dispatch("help board status") == (True, "board status  Report board status.")
assert bare_registry.dispatch("help show pipe") == (
    True, "show pipes (show pipe)  List matching configured Pipes."
)
ok, prefix_out = bare_registry.dispatch("help show")
assert ok and "show actions (show action)" in prefix_out and "show pipes (show pipe)" in prefix_out
assert bare_registry.dispatch("help missing") == (False, "unknown action")
assert bare_registry.help_for("show pipes") == "List matching configured Pipes."
assert bare_registry.help_for("show pipe") == "List matching configured Pipes."

# Custom action aliases (explicit name and bare function) and remote dispatch via alias.
@bare_registry.make_action("node check", help="Check node.", alias="ping")
def node_check(args):
    return True, "pong:" + ",".join(args)


@bare_registry.make_action(alias="stat", help="Report quick status.")
def quick_status(args):
    return True, "green"


assert bare_registry.dispatch("ping one two") == (True, "pong:one,two")
assert bare_registry.dispatch("stat") == (True, "green")
assert bare_registry.dispatch("help ping") == (True, "node check (ping)  Check node.")
assert bare_registry.dispatch("help stat") == (True, "quick status (stat)  Report quick status.")
assert bare_registry.dispatch("ping a", ("node check",)) == (True, "pong:a")
assert not bare_registry.dispatch("ping a", ("help",))[0]
assert bare_registry.dispatch("help ping", ("help",)) == (False, "unknown action")
assert bare_registry.names(("node check", "show pipes")) == ["node check", "show pipes"]

# Non-default instance Pipe(...) formatting and multi-pipe partial failure markers.
multi_reg = ActionRegistry()
good_src = provision("plugin:test_source", "temp", root + "/multi-good.mm", 1, 31, 2,
                     instance="journal:alt", create=True)
bad_src = provision("plugin:test_source", "fail", root + "/multi-bad.mm", 1, 32, 2,
                    location="west", instance="journal:alt", create=True)
multi_reg.expose(good_src, read=True, write=True)
multi_reg.expose(bad_src, read=True, write=True)
ok, shown = multi_reg.dispatch("show pipes -i journal:alt")
assert ok and shown == (
    "Pipe('plugin:test_source', 'temp', instance='journal:alt')\n"
    "Pipe('plugin:test_source', 'fail', 'west', instance='journal:alt')"
), shown


def flaky_fetch(pipe):
    if pipe.metric_key == "fail":
        raise RuntimeError("sensor offline")
    return {"v": 7}


test_source.fetch = flaky_fetch
ok, sync_out = multi_reg.dispatch("sync pipes -m temp fail")
assert not ok, sync_out
assert sync_out == (
    "+ Pipe('plugin:test_source', 'temp', instance='journal:alt')  committed 1 input row\n"
    "- Pipe('plugin:test_source', 'fail', 'west', instance='journal:alt')  sensor offline (confirmed 0 input rows)"
), sync_out
good_src.close()
bad_src.close()

# Reject invalid/duplicate bare, explicit, and alias registrations.
for bad in (
    lambda: bare_registry.make_action(lambda args: (True, "x")),
    lambda: bare_registry.make_action(device_label),
    lambda: bare_registry.make_action("bad  name"),
    lambda: bare_registry.make_action("valid name", help=123),
    lambda: bare_registry.make_action("valid name", alias="bad  alias"),
    lambda: bare_registry.make_action("same", alias="same"),
    lambda: bare_registry.make_action("other", alias="ping"),
    lambda: bare_registry.make_action("ping"),
    lambda: bare_registry.make_action("other", alias="show pipe"),
    lambda: bare_registry.make_action("show pipe"),
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

# Interactive shell polish: dynamic instance prompt & scoping, ANSI/Unicode, clear, ?, history.
sh_reg = ActionRegistry()
sh_seen = []


@sh_reg.make_action("note", help="Record a note.")
def sh_note(args):
    sh_seen.append(args)
    return True, " ".join(args) if args else "empty"


@sh_reg.make_action("boom")
def sh_boom(args):
    return False, "kapow"


p_local = provision("sensor", "alpha", root + "/sh-local.mm", 1, 41, 2, create=True)
p_alt = provision("sensor", "beta", root + "/sh-alt.mm", 1, 42, 2, instance="journal:alt", create=True)
sh_reg.expose(p_local, read=True, write=True)
sh_reg.expose(p_alt, read=True, write=True)

# 1. Dynamic instance switching, automatic -i scoping, explicit -i override, reset, clear, ?.
out = io.StringIO()
sh_reg.shell(io.StringIO(
    "show pipes\n"
    "instance\n"
    "instance journal:alt\n"
    "show pipes\n"
    "show pipe -i journal:local\n"
    "instance bad space\n"
    "instance all\n"
    "show pipes\n"
    "clear\n"
    "?\n"
    "zzz?\n"
    "boom\n"
    "exit\n"
), out)
text = out.getvalue()
assert text.startswith("MicroMeerschaum 0.2.1\n[ journal:local ] > "), text
assert "+ Pipe('sensor', 'alpha')\n+ Pipe('sensor', 'beta', instance='journal:alt')\n" in text, text
assert "+ journal:local\n" in text, text
assert "+ Active instance: journal:alt\n[ journal:alt ] > " in text, text
# Scoped show pipes under journal:alt omits alpha.
after_switch = text.split("[ journal:alt ] > show pipes\n", 1)[1].split("[ journal:alt ] > ", 1)[0]
assert after_switch == "+ Pipe('sensor', 'beta', instance='journal:alt')\n", after_switch
# Explicit -i overrides active instance.
after_override = text.split("show pipe -i journal:local\n", 1)[1].split("[ journal:alt ] > ", 1)[0]
assert after_override == "+ Pipe('sensor', 'alpha')\n", after_override
assert "- Usage: instance [KEYS|all|default|reset]\n" in text, text
assert "+ Active instance: journal:local\n[ journal:local ] > " in text, text
assert "\x1b[2J\x1b[H[ journal:local ] > " in text, text
assert "+ note\n" in text and "- no matching actions\n" in text and "- kapow\n" in text, text

# 2. ANSI + Unicode styling mode.
styled = io.StringIO()
sh_reg.shell(
    io.StringIO("note ok\nboom\nexit\n"),
    styled,
    instance="journal:alt",
    ansi=True,
    unicode=True,
)
styled_text = styled.getvalue()
assert styled_text.startswith("MicroMeerschaum 0.2.1\n\x1b[32m[\x1b[0m \x1b[36mjournal:alt\x1b[0m \x1b[32m] \u27a4\x1b[0m "), styled_text
assert "\x1b[32m+ \x1b[0mok\n" in styled_text, styled_text
assert "\x1b[31m- \x1b[0mkapow\n" in styled_text, styled_text

# 3. Bounded VT100 history ring buffer (size=3), deduplication, Up/Down recall, CSI ignore.
hist_out = io.StringIO()
sh_seen.clear()
sh_reg.shell(
    io.StringIO(
        "note one\n"
        "note two\n"
        "note two\n"     # consecutive duplicate deduplicated
        "note three\n"
        "note four\n"    # evicts "note one" (history_size=3 -> [two, three, four])
        "\x1b[A\x1b[A\x1b[B\n"           # Up(four), Up(three), Down(four) -> runs four
        "\x1b[A\x1b[A\x1b[A\x1b[A\n"     # Up x4 clamps at oldest (two) -> runs two
        "\x1b[A\x1b[B\n"                 # Up(two) then Down("") -> empty line, no dispatch
        "note \x1b[D\x1b[C\x1b[3~clean\n"  # other CSI sequences ignored cleanly
        "exit\n"
    ),
    hist_out,
    history_size=3,
)
assert sh_seen == [["one"], ["two"], ["two"], ["three"], ["four"], ["four"], ["two"], ["clean"]], sh_seen
assert "\b \b" in hist_out.getvalue()

# 4. Recalled pipe command uses the newly switched instance, not the old one.
re_out = io.StringIO()
sh_reg.shell(
    io.StringIO("instance journal:alt\nshow pipes\ninstance journal:local\n\x1b[A\x1b[A\nexit\n"),
    re_out,
)
re_text = re_out.getvalue()
re_tail = re_text.rsplit("[ journal:local ] > ", 2)[1]
assert "+ Pipe('sensor', 'alpha')\n" in re_tail and "beta" not in re_tail, re_tail

# 5. Reject invalid shell options.
for bad_call in (
    lambda: sh_reg.shell(io.StringIO(""), io.StringIO(), instance=""),
    lambda: sh_reg.shell(io.StringIO(""), io.StringIO(), instance="bad space"),
    lambda: sh_reg.shell(io.StringIO(""), io.StringIO(), history_size=-1),
):
    try:
        bad_call()
        raise AssertionError("accepted invalid shell() option")
    except ValueError:
        pass

p_local.close()
p_alt.close()
print("interactive shell greeting, dynamic prompt, instance scoping, ANSI/Unicode, clear, ?, and history")

# Built-in inspection actions: show data/columns/rowcounts/progress/connectors/version.
insp_reg = ActionRegistry()
jp = provision("sensor", "inspect_j", root + "/inspect-j.mm", 1, 41, 2, create=True)
jp_noread = provision("sensor", "inspect_nr", root + "/inspect-nr.mm", 1, 42, 2,
                      location="east", create=True)
jp_hidden = provision("sensor", "inspect_hidden", root + "/inspect-hid.mm", 1, 43, 2,
                      instance="journal:secret", create=True)
assert jp.sync([{"tick": 1, "note": "a"}, {"tick": 2, "note": "b"}, {"tick": 3, "note": "c"}])[0]
assert jp_noread.sync([{"tick": 9}])[0]
assert jp_hidden.sync([{"tick": 99}])[0]
insp_reg.expose(jp, read=True, write=True)
insp_reg.expose(jp_noread, read=False, write=False)

# show version + help/aliases
ok, ver = insp_reg.dispatch("show version")
assert ok and ver == "MicroMeerschaum 0.2.1 (MicroPython)", ver
assert not insp_reg.dispatch("show version extra")[0]
assert insp_reg.dispatch("help show datum")[0] and "show data (show datum)" in insp_reg.dispatch("help show datum")[1]
assert insp_reg.dispatch("help show column")[0] and "show columns (show column)" in insp_reg.dispatch("help show column")[1]
assert insp_reg.dispatch("help show rowcount")[0] and "show rowcounts (show rowcount)" in insp_reg.dispatch("help show rowcount")[1]
assert insp_reg.dispatch("help show connector")[0] and "show connectors (show connector)" in insp_reg.dispatch("help show connector")[1]
assert insp_reg.dispatch("help show progress")[0] and "show progress" in insp_reg.dispatch("help show progress")[1]
assert insp_reg.dispatch("help show version")[0] and "show version" in insp_reg.dispatch("help show version")[1]

# show data / show datum on journal pipes
ok, out = insp_reg.dispatch("show data -m inspect_j --limit 2")
assert ok and out.startswith("Pipe('sensor', 'inspect_j')\n") and "'tick': 1" in out and "'tick': 2" in out and "'tick': 3" not in out, out
ok, out = insp_reg.dispatch("show datum -m inspect_j --limit 0")
assert ok and out == "Pipe('sensor', 'inspect_j')\n  (no rows)", out
assert not insp_reg.dispatch("show data -m inspect_j --begin 1")[0]
assert not insp_reg.dispatch("show data -m inspect_j --end 2")[0]
for bad_data in (
    "show data --limit 129",
    "show data --limit -1",
    "show data --limit nope",
    "show data --limit 1 --limit 2",
    "show data --begin",
    "show data --begin -c sensor",
    "show data --begin 1 --begin 2",
    "show data --end",
    "show data --end 1 --end 2",
    "show data --unknown 1",
):
    assert not insp_reg.dispatch(bad_data)[0], bad_data

# show columns / show rowcounts / show progress on journal pipes
ok, out = insp_reg.dispatch("show column -m inspect_j")
assert ok and out == "Pipe('sensor', 'inspect_j')\n  (no fixed schema; journal pipe)", out
ok, out = insp_reg.dispatch("show rowcount -m inspect_j")
assert ok and out == "Pipe('sensor', 'inspect_j')  3", out
ok, out = insp_reg.dispatch("show progress -m inspect_j")
assert ok and out == "Pipe('sensor', 'inspect_j')  prefix=3 high=3 confirmed=0 missed=0", out
ok, out = insp_reg.dispatch("show data -c nonexistent")
assert ok and out == "no matching Pipes", out

# show connectors / show connector (local sees hidden journal:secret; remote does NOT)
ok, out = insp_reg.dispatch("show connectors")
assert ok and "journal:local" in out and "journal:secret" in out and "peer:archive" in out, out
ok, out = insp_reg.dispatch("show connector peer")
assert ok and out == "peer:archive", out
ok, out = insp_reg.dispatch("show connector api")
assert ok and out == "no matching connectors", out
assert not insp_reg.dispatch("show connectors bogus")[0]
assert not insp_reg.dispatch("show connectors peer extra")[0]

# Bounded reply truncation for large journal rows (fits within 4096-byte session frame)
jp_big = provision("sensor", "inspect_big", root + "/inspect-big.mm", 1, 44, 2, create=True)
assert jp_big.sync({"text": ["x" * 1400] * 3})[0]
insp_reg.expose(jp_big, read=True, write=False)

# Remote allowlist and read-permission enforcement via DeviceSession
deny_session = DeviceSession(insp_reg, ("action",), ("help",))
for cmd in (
    "show data", "show datum", "show columns", "show column",
    "show rowcounts", "show rowcount", "show progress",
    "show connectors", "show connector", "show version",
):
    rep = json.loads(deny_session.handle(frame("action", line=cmd)[:-1])[len(PREFIX):])
    assert not rep["ok"], (cmd, rep)

inspect_session = DeviceSession(
    insp_reg,
    ("action",),
    ("show data", "show columns", "show rowcounts", "show progress", "show connectors", "show version"),
)


def icall(line):
    raw = inspect_session.handle(frame("action", line=line)[:-1])
    assert raw is not None and len(raw) <= 4096, (line, len(raw) if raw else None)
    return json.loads(raw[len(PREFIX):])


assert icall("show version") == {"v": 1, "id": "123", "ok": True, "data": "MicroMeerschaum 0.2.1 (MicroPython)"}
conn_rep = icall("show connectors")
assert conn_rep["ok"] and "journal:local" in conn_rep["data"] and "peer:archive" in conn_rep["data"], conn_rep
assert "journal:secret" not in conn_rep["data"], conn_rep
assert icall("show data -m inspect_j --limit 1")["ok"]
assert icall("show datum -m inspect_j --limit 1")["ok"]
assert icall("show columns -m inspect_j")["ok"]
assert icall("show rowcounts -m inspect_j")["data"] == "Pipe('sensor', 'inspect_j')  3"
assert icall("show progress -m inspect_j")["data"] == "Pipe('sensor', 'inspect_j')  prefix=3 high=3 confirmed=0 missed=0"
# Oversized journal output truncates cleanly within DeviceSession frame limit
big_rep = icall("show data -m inspect_big --limit 3")
assert big_rep["ok"] and "... (truncated)" in big_rep["data"], big_rep
# Remote read=False pipe is denied on all pipe-scoped inspection actions
for cmd in (
    "show data -l east", "show columns -l east", "show rowcounts -l east", "show progress -l east",
    "show data", "show rowcounts",
):
    rep = icall(cmd)
    assert not rep["ok"] and "denied" in rep["error"].lower(), (cmd, rep)
# Hidden/unexposed pipe is never disclosed
assert icall("show data -m inspect_hidden")["data"] == "no matching Pipes"
assert icall("show rowcounts -i journal:secret")["data"] == "no matching Pipes"

for pipe in (jp, jp_noread, jp_hidden, jp_big):
    pipe.close()
print("built-in inspection actions (journal, remote allowlist, read gating, bounded truncation)")
