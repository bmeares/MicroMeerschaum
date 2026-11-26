"""Qualify the installed micro plugin with unmodified Meerschaum CLI and Pipe APIs on Linux."""

import argparse
import json
import os
import pty
import re
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tty
from pathlib import Path

PLUGIN = Path(__file__).resolve().parents[1]
DEFAULT_KEY = ["sensor", "readings", None]
READ_ONLY = ["sensor", "readonly", "outside"]
OTHER_KEY = ["sensor", "other", None]


class Device:
    def __init__(self):
        self.fd, self.slave = pty.openpty()
        tty.setraw(self.slave)
        self.port = os.ttyname(self.slave)
        self.rows = []
        self.writes = 0
        self.other_rows = []
        self.actions = []
        self.running = True
        self.worker = threading.Thread(target=self.serve, daemon=True)
        self.worker.start()

    def close(self):
        self.running = False
        self.worker.join(2)
        os.close(self.fd)
        os.close(self.slave)

    def serve(self):
        pending = b""
        while self.running:
            if not select.select([self.fd], [], [], 0.1)[0]:
                continue
            pending += os.read(self.fd, 8192)
            while b"\n" in pending:
                packet, pending = pending.split(b"\n", 1)
                assert packet.startswith(b"@micro ")
                req = json.loads(packet[7:])
                data, failure = None, None
                if req["op"] == "catalog":
                    items = [
                        dict(
                            id=i + 1,
                            key=key,
                            instance="journal:local",
                            read=True,
                            write=key != READ_ONLY,
                        )
                        for i, key in enumerate((DEFAULT_KEY, READ_ONLY, OTHER_KEY))
                    ]
                    offset = req.get("offset", 0)
                    # Exercise page continuation even when the host requested more entries.
                    data = dict(
                        items=items[offset : offset + 1], more=offset + 1 < len(items)
                    )
                elif req["op"] == "write":
                    self.writes += 1
                    assert req["key"] in (DEFAULT_KEY, OTHER_KEY)
                    rows = self.rows if req["key"] == DEFAULT_KEY else self.other_rows
                    simulate = req["rows"][0].get("simulate")
                    if simulate == "partial":
                        rows.extend(req["rows"][:1])
                        failure = "device storage error (confirmed 1 input row)"
                    else:
                        rows.extend(req["rows"])
                        if simulate == "go offline":
                            os.unlink(self.port)
                            continue
                        if simulate == "drop":
                            continue
                        data = "Stored %d rows" % len(req["rows"])
                elif req["op"] == "read":
                    data = (
                        self.rows[: req["limit"]]
                        if req["key"] == DEFAULT_KEY
                        else [{"tick": 1, "value": 47}][: req["limit"]]
                    )
                elif req["op"] == "action":
                    self.actions.append(req["line"])
                    if req["line"] == "disconnect":
                        continue
                    if req["line"] == "refuse":
                        failure = "Device refused this action"
                    else:
                        data = "Ran " + req["line"]
                elif req["op"] == "complete":
                    data = [
                        x for x in ("show pipes", "help") if x.startswith(req["line"])
                    ]
                else:
                    raise ValueError(req)
                reply = dict(v=1, id=req["id"], ok=not failure)
                if failure:
                    reply["error"] = failure
                else:
                    reply["data"] = data
                os.write(self.fd, b"@micro " + json.dumps(reply).encode() + b"\n")


def command(*argv, ok=True):
    result = subprocess.run(
        ["mrsm", *argv],
        cwd=os.environ["MRSM_ROOT_DIR"],
        capture_output=True,
        timeout=40,
    )
    report = (result.stdout + result.stderr).decode()
    assert (result.returncode == 0) == ok, (argv, report)
    return report


def _open_shell_pty(extra_env=None, cols=80, rows=24):
    import fcntl
    import struct
    import termios

    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    proc = subprocess.Popen(
        ["mrsm", "micro", "shell", "-i", "micro:bench"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
        env=env,
    )
    os.close(slave)
    transcript = b""
    full_output = bytearray()

    def read_chunk(deadline):
        ready = select.select([master], [], [], max(0, deadline - time.monotonic()))[0]
        assert ready, (bytes(full_output), transcript)
        chunk = os.read(master, 8192)
        if b"\x1b[6n" in chunk:
            os.write(master, b"\x1b[1;1R")
        full_output.extend(chunk)
        return chunk

    def until(expected):
        nonlocal transcript
        end = time.monotonic() + 12
        while expected not in transcript:
            transcript += read_chunk(end)
        before, transcript = transcript.split(expected, 1)
        return before + expected

    def wait_prompt():
        # Wait through the prompt and the end of Reedline's paint frame (including toolbar).
        until(b"micro:bench")
        return until(b"\x1b[?25h")

    def drain_exit():
        end = time.monotonic() + 12
        while proc.poll() is None and time.monotonic() < end:
            if select.select([master], [], [], 0.05)[0]:
                try:
                    chunk = os.read(master, 8192)
                except OSError:
                    break
                if not chunk:
                    break
                full_output.extend(chunk)
        assert proc.wait(12) == 0, bytes(full_output)
        while select.select([master], [], [], 0.1)[0]:
            try:
                chunk = os.read(master, 8192)
            except OSError:
                break
            if not chunk:
                break
            full_output.extend(chunk)
        return bytes(full_output)

    return master, proc, until, wait_prompt, drain_exit, full_output


def shell(device):
    # 1. Interactive styled TTY session: banner, styled prompt, toolbar, icons, conveniences,
    #    history persistence, and clean close message.
    before_actions = list(device.actions)
    master, proc, until, wait_prompt, drain_exit, _ = _open_shell_pty()
    try:
        frame = wait_prompt()
        assert b"serial" in frame and b"disconnected" in frame, frame
        os.write(master, b"abandon\x03")
        wait_prompt()
        os.write(master, b"sh\t")
        until(b"show pipes")
        os.write(master, b"\r")
        until(b"Ran show pipes")
        frame = wait_prompt()
        assert b"serial" in frame and b"connected" in frame, frame

        # Failure reply uses styled failure icon.
        os.write(master, b"refuse\r")
        until(b"Device refused this action")
        wait_prompt()

        # Local conveniences: clear, ?, <prefix>?, --help, leading mrsm stripping.
        os.write(master, b"clear\r")
        until(b"\x1b[2J\x1b[H")
        wait_prompt()

        os.write(master, b"?\r")
        until(b"show pipes")
        until(b"help")
        wait_prompt()

        os.write(master, b"sh?\r")
        until(b"show pipes")
        wait_prompt()

        os.write(master, b"show pipes --help\r")
        until(b"Ran help show pipes")
        wait_prompt()

        os.write(master, b"--help\r")
        until(b"Ran help")
        wait_prompt()

        os.write(master, b"mrsm show pipes\r")
        until(b"Ran show pipes")
        wait_prompt()

        os.write(master, b"quit\r")
        output = drain_exit()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)

    assert b"MicroMeerschaum" in output, output
    assert b"\x1b[1;94m" in output, output  # intro style (bold bright_blue)
    assert b"\x1b[32m" in output and b"\x1b[36mmicro:bench\x1b[0m" in output, output
    assert b"\x1b[1;92m" in output and "\U0001f389".encode() in output, output
    assert b"\x1b[1;31m" in output and "\U0001f4a2".encode() in output, output
    assert b"\x1b[94m" in output and b"Thank you for using" in output, output
    assert b"Device shell closed." not in output, output

    # Confirm local commands (clear, ?, sh?) were not sent as device actions.
    assert device.actions[len(before_actions) :] == [
        "show pipes",
        "refuse",
        "help show pipes",
        "help",
        "show pipes",
    ], device.actions[len(before_actions) :]

    # Verify history persisted under XDG_STATE_HOME/micromeerschaum/shell_history.
    history_file = Path(os.environ["XDG_STATE_HOME"]) / "micromeerschaum" / "shell_history"
    assert history_file.is_file(), history_file
    history_lines = history_file.read_text().splitlines()
    assert "mrsm show pipes" in history_lines and "show pipes --help" in history_lines, history_lines

    # 2. Second PTY session: recall persistent history via Up arrow & Ctrl-R, exit via Ctrl-D.
    before_second = len(device.actions)
    master, proc, until, wait_prompt, drain_exit, _ = _open_shell_pty()
    try:
        wait_prompt()
        # Up twice skips "quit" and recalls "mrsm show pipes".
        os.write(master, b"\x1b[A\x1b[A")
        until(b"mrsm show pipes")
        os.write(master, b"\r")
        until(b"Ran show pipes")
        wait_prompt()
        # Ctrl-R searches persistent history for "--help" -> "show pipes --help".
        os.write(master, b"\x12show pipes --h")
        until(b"show pipes --help")
        # First Enter exits reverse-search with the match in the buffer; second Enter executes it.
        os.write(master, b"\r")
        until(b"\x1b[?25h")
        os.write(master, b"\r")
        until(b"Ran help show pipes")
        wait_prompt()
        # Clean exit via Ctrl-D prints the close message.
        os.write(master, b"\x04")
        output2 = drain_exit()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)

    assert b"Thank you for using" in output2, output2
    assert device.actions[before_second:] == ["show pipes", "help show pipes"], device.actions[
        before_second:
    ]

    # 3. Interactive TTY with NO_COLOR=1 and narrow terminal: no SGR color escapes.
    master, proc, until, wait_prompt, drain_exit, _ = _open_shell_pty(
        extra_env={"NO_COLOR": "1"}, cols=44, rows=24
    )
    try:
        wait_prompt()
        os.write(master, b"show pipes\r")
        until(b"Ran show pipes")
        wait_prompt()
        os.write(master, b"exit\r")
        nocolor_output = drain_exit()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)

    assert b"MicroMeerschaum" in nocolor_output, nocolor_output
    assert b"(no userna\xe2\x80\xa6@micro:bench" in nocolor_output, nocolor_output
    assert b"Thank you for using" in nocolor_output, nocolor_output
    # Zero ANSI style/color SGR codes (Reedline only emits bare \x1b[0m resets before hiding cursor).
    assert re.search(rb"\x1b\[(?:[1-9][0-9;]*|0;[0-9;]+)m", nocolor_output) is None, nocolor_output

    # 3b. Interactive TTY with formatting.ansi=false & formatting.unicode=false (ASCII mode).
    ascii_cfg = json.loads(os.environ["MRSM_CONFIG"])
    ascii_cfg["formatting"] = {"ansi": False, "unicode": False}
    master, proc, until, wait_prompt, drain_exit, _ = _open_shell_pty(
        extra_env={"MRSM_CONFIG": json.dumps(ascii_cfg)}, cols=80, rows=24
    )
    try:
        frame = wait_prompt()
        assert b" ] > " in frame and b"serial" in frame and b"disconnected" in frame, frame
        os.write(master, b"show pipes\r")
        until(b" + Ran show pipes")
        wait_prompt()
        os.write(master, b"exit\r")
        ascii_output = drain_exit()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)

    assert b"MicroMeerschaum" in ascii_output and b"Thank you for using" in ascii_output, ascii_output
    assert re.search(rb"\x1b\[(?:[1-9][0-9;]*|0;[0-9;]+)m", ascii_output) is None, ascii_output

    # 4. Piped (non-TTY) session: plain output, no banner, no close message, no toolbar, no ANSI.
    piped_before = len(device.actions)
    piped = subprocess.run(
        ["mrsm", "micro", "shell", "-i", "micro:bench"],
        cwd=os.environ["MRSM_ROOT_DIR"],
        input="show pipes\nclear\nsh?\nshow pipes --help\nmrsm show pipes\nexit\n",
        capture_output=True,
        text=True,
        timeout=40,
    )
    assert piped.returncode == 0, (piped.stdout, piped.stderr)
    assert piped.stdout == "Ran show pipes\nshow pipes\nRan help show pipes\nRan show pipes\n", (
        piped.stdout,
        piped.stderr,
    )
    assert piped.stderr == "", piped.stderr
    assert "\x1b" not in piped.stdout and "\x1b" not in piped.stderr
    assert "MicroMeerschaum" not in piped.stdout and "Thank you" not in piped.stdout
    assert device.actions[piped_before:] == [
        "show pipes",
        "help show pipes",
        "show pipes",
    ], device.actions[piped_before:]


def qualify(root, library, transport_python):
    plugins = root / "plugins"
    package = plugins / "micro"
    package.mkdir(parents=True)
    shutil.copy2(library, package / library.name)
    shutil.copy2(PLUGIN / "plugin.toml", package)
    device = Device()
    port = root / "serial-port"
    port.symlink_to(device.port)
    device.port = str(port)
    for name in list(os.environ):
        if name.startswith("MRSM_"):
            del os.environ[name]
    config = {
        "meerschaum": {
            "instance": "sql:main",
            "executor": "local",
            "connectors": {
                "micro": {
                    "bench": {
                        "transport": "serial",
                        "port": device.port,
                        "python": transport_python,
                        "timeout": 0.5,
                    }
                },
                "sql": {
                    "main": {"flavor": "sqlite", "database": str(root / "pipes.db")}
                },
            },
        }
    }
    os.environ.update(
        XDG_STATE_HOME=str(root / "state"),
        MRSM_ROOT_DIR=str(root),
        MRSM_PLUGINS_DIR=str(plugins),
        MRSM_NO_AUTO_INSTALL="1",
        MRSM_NO_UPDATE_CHECK="1",
        MRSM_CONFIG=json.dumps(config),
    )
    try:
        import meerschaum as mrsm

        print("Qualifying Meerschaum", mrsm.__version__, flush=True)
        report = command("show", "pipes", "-i", "micro:bench")
        assert "readings" in report and "readonly" in report, report
        report = command("show", "pipes", "-m", "readonly", "-i", "micro:bench")
        assert "readonly" in report and "readings" not in report, report

        remote = mrsm.Pipe(*DEFAULT_KEY, instance="micro:bench")
        dest = mrsm.Pipe(*DEFAULT_KEY, instance="sql:main")
        readonly = mrsm.Pipe(*READ_ONLY, instance="micro:bench")
        missing = mrsm.Pipe("sensor", "missing", instance="micro:bench")
        assert (
            remote.exists()
            and remote.id == 1
            and not missing.exists()
            and missing.id is None
        )
        assert not readonly.sync([{"value": 0}])[0]
        assert not missing.sync([{"value": 0}])[0]
        assert not device.writes
        rows = [{"tick": 1, "value": 3}, {"tick": 2, "value": 4}]
        ok, msg = dest.sync(rows)
        assert ok, msg
        report = command(
            "copy",
            "pipes",
            "micro:bench",
            "-i",
            "sql:main",
            "-m",
            "readings",
            "--sync-data",
            "incremental",
            "-y",
        )
        assert remote.get_docs(limit=2) == rows, report
        ok, msg = remote.sync(dest.get_docs(limit=1))
        assert ok and device.rows == rows + rows[:1], msg
        assert remote.get_docs(limit=1) == rows[:1]
        assert remote.get_docs(limit=0) == []
        assert remote.get_docs(select_columns=["value"], limit=2) == [
            {"value": 3},
            {"value": 4},
        ]
        archive = mrsm.Pipe("archive", "from_device", instance="sql:main")
        ok, msg = archive.sync(remote.get_docs(limit=2))
        assert ok and archive.get_docs(limit=2) == rows, msg
        report = command(
            "copy",
            "pipes",
            "sql:main",
            "-i",
            "micro:bench",
            "-m",
            "readonly",
            "--sync-data",
            "all",
            "-y",
        )
        assert mrsm.Pipe(*READ_ONLY, instance="sql:main").id is not None, report
        # A new SQL instance, including the regular CLI's registration and row-copy path.
        report = command(
            "copy",
            "pipes",
            "sql:local",
            "-i",
            "micro:bench",
            "-m",
            "readings",
            "--sync-data",
            "all",
            "-y",
        )
        assert (
            mrsm.Pipe(*DEFAULT_KEY, instance="sql:local").get_docs(limit=2) == rows
        ), report

        guard_tests(
            remote,
            mrsm.Pipe(*OTHER_KEY, instance="micro:bench"),
            device,
            config,
            transport_python,
        )
        assert (
            command("micro", "action", "show", "pipes", "-i", "micro:bench").find(
                "Ran show pipes"
            )
            >= 0
        )
        assert "Ran show pipes -m readings" in command(
            "micro", "action", "show pipes -m readings", "-i", "micro:bench"
        )
        assert "Device refused" in command(
            "micro", "action", "refuse", "-i", "micro:bench", ok=False
        )
        assert (
            "unknown"
            in command(
                "micro", "action", "disconnect", "-i", "micro:bench", ok=False
            ).lower()
        )
        shell(device)
        assert device.actions[:4] == [
            "show pipes",
            "show pipes -m readings",
            "refuse",
            "disconnect",
        ], device.actions
        print(
            "Passed native discovery, SQL transfers, bounded reads, permissions, shell, forced retries, concurrent writes and reconciliation."
        )
    finally:
        device.close()


def guard_tests(remote, other, device, config, python):
    def recovery_token(message):
        match = re.search(r"mrsm micro reconcile ([0-9a-f.]+) ", message)
        assert match, message
        return match[1]

    def reconcile(token):
        return command("micro", "reconcile", token, "-i", "micro:bench")

    before = device.writes
    ok, msg = remote.sync([{"simulate": "drop"}], force=True, retries=3, min_seconds=0)
    assert not ok and "Timed out" in msg and "unknown" in msg.lower(), msg
    assert device.writes == before + 1
    token = recovery_token(msg)
    assert " -i micro:bench " in msg, msg
    assert remote.get_docs(limit=10)[-1]["simulate"] == "drop"
    assert other.sync([{"value": 73}])[0] and device.other_rows == [{"value": 73}]
    assert "Reconciliation recorded" in reconcile(token)
    assert remote.sync([{"value": 11}])[0]

    before = device.writes
    ok, msg = remote.sync(
        [{"simulate": "partial"}, {"value": 2}], force=True, retries=2, min_seconds=0
    )
    assert not ok and "confirmed 1 input row" in msg and device.writes == before + 1, (
        msg
    )
    assert "No matching" in command(
        "micro", "reconcile", token, "-i", "micro:bench", ok=False
    )
    assert "Reconciliation recorded" in reconcile(recovery_token(msg))

    # The device disappears after committing. Its catalog cannot answer the forced retry.
    before = device.writes
    ok, msg = remote.sync(
        [{"simulate": "go offline"}], force=True, retries=2, min_seconds=0
    )
    assert not ok and "unknown" in msg.lower() and device.writes == before + 1, msg
    assert "Reconciliation recorded" in reconcile(recovery_token(msg))
    ok, msg = remote.sync([{"value": 42}])
    assert not ok and "could not open port" in msg and "reconcile" not in msg, msg
    os.symlink(os.ttyname(device.slave), device.port)

    # An oversized request fails locally and leaves the Pipe available.
    ok, msg = remote.sync([{"value": "x" * 4096}])
    assert not ok and "exceeds 4096" in msg, msg
    assert remote.sync([{"value": 12}])[0]

    # Isolate helper-process tests from Meerschaum's background metadata reads.
    isolated = Device()
    try:
        settings = dict(
            config["meerschaum"]["connectors"]["micro"]["bench"], port=isolated.port
        )
        helper_guard_tests(isolated, settings, python, recovery_token)
    finally:
        isolated.close()


def helper_guard_tests(device, settings, python, recovery_token):
    def helper(request):
        child = subprocess.Popen(
            [python, str(PLUGIN / "src/transport.py")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
        )
        child.stdin.write(json.dumps({"config": settings, "request": request}) + "\n")
        child.stdin.flush()
        return child

    def reconcile(token):
        output = helper({"op": "reconcile", "token": token}).communicate(timeout=10)[0]
        assert json.loads(output)["ok"], output

    # The second contender must see the first helper's incident and original error.
    dropped = {"op": "write", "key": DEFAULT_KEY, "rows": [{"simulate": "drop"}]}
    procs = [helper(dropped) for _ in range(2)]
    reports = [
        [json.loads(line) for line in proc.communicate(timeout=10)[0].splitlines()]
        for proc in procs
    ]
    msgs = [r[-1]["error"] for r in reports]
    assert msgs[0] == msgs[1] and device.writes == 1, msgs
    reconcile(recovery_token(msgs[0]))

    # Losing the parent before it receives the result leaves the incident pending.
    write = {"op": "write", "key": DEFAULT_KEY, "rows": [{"value": 13}]}
    proc = helper(write)
    incident = recovery_token(json.loads(proc.stdout.readline())["pending"])
    assert json.loads(proc.stdout.readline())["ok"]
    proc.communicate(timeout=10)
    result = json.loads(helper(write).communicate(timeout=10)[0])
    assert (
        not result["ok"]
        and recovery_token(result["error"]) == incident
        and device.writes == 2
    )
    reconcile(incident)


def device_rows():
    import meerschaum as mrsm

    remote = mrsm.Pipe("sensor", "temperature", instance="micro:device")
    source = mrsm.Pipe("station", "source", instance="sql:main")
    assert source.sync([{"tick": 15, "value": 43}, {"tick": 20, "value": 50}])[0]
    ok, message = remote.sync(source.get_docs(limit=2))
    assert ok, message
    ok, message = remote.sync([{"tick": 20, "value": 55}])
    assert ok, message
    filtered = remote.get_docs(begin=18, end=22, params={"value": 55}, limit=1)
    assert filtered == [{"tick": 20, "value": 55}], filtered
    local = mrsm.Pipe("station", "pull", instance="sql:main")
    assert local.sync(remote.get_docs(limit=2))[0]
    assert local.get_docs(limit=2) == [
        {"tick": 15, "value": 43},
        {"tick": 20, "value": 55},
    ]
    from datetime import datetime, timezone

    stamp = datetime(2024, 6, 3, 16, 5, 1, 123456, tzinfo=timezone.utc)
    journal = mrsm.Pipe("sensor", "audit", instance="micro:device")
    ok, message = journal.sync([{"when": stamp, "event": "sample"}])
    assert ok, message
    assert journal.get_docs(limit=1) == [{"when": stamp, "event": "sample"}]


def micropython(root, executable, modules, python):
    """Use the production session implementation and keyed SQLite store on a serial PTY."""
    master, slave = pty.openpty()
    tty.setraw(slave)
    proc = subprocess.Popen(
        [str(executable), str(PLUGIN / "tests/device_node.py"), str(root)],
        stdin=master,
        stdout=master,
        stderr=subprocess.PIPE,
        cwd=root,
        env=dict(os.environ, MICROPYPATH=str(modules) + ":.frozen"),
    )
    try:
        for _ in range(100):
            if proc.poll() is not None:
                raise RuntimeError(proc.stderr.read().decode())
            if (root / "device.ready").exists():
                break
            time.sleep(0.05)
        assert (root / "device.ready").exists()
        settings = {
            "transport": "serial",
            "port": os.ttyname(slave),
            "python": python,
            "timeout": 3,
        }
        overlay = json.loads(os.environ["MRSM_CONFIG"])
        overlay["meerschaum"]["connectors"]["micro"]["device"] = settings
        os.environ["MRSM_CONFIG"] = json.dumps(overlay)
        # Start the ordinary host consumer with its configuration present at import.
        subprocess.run(
            [
                sys.executable,
                "-c",
                "from device_instance import device_rows; device_rows()",
            ],
            check=True,
            cwd=PLUGIN / "tests",
            timeout=60,
        )
        assert "temperature" in command(
            "micro", "action", "show", "pipes", "-i", "micro:device"
        )
        assert "temperature" in command("show", "pipes", "-i", "micro:device")
        print(
            "Passed host SQL → device SQLite → host SQL with bounded filtered reads and an upsert."
        )
    finally:
        proc.terminate()
        proc.wait(5)
        os.close(slave)
        os.close(master)


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--library", type=Path, required=True)
    cli.add_argument(
        "--python", default=sys.executable, help="Interpreter with pyserial installed."
    )
    cli.add_argument(
        "--micropython",
        type=Path,
        help="Built Unix MicroPython with _mm and _mmsqlite.",
    )
    cli.add_argument(
        "--modules", type=Path, default=PLUGIN.parents[1] / "ports/micropython"
    )
    args = cli.parse_args()
    with tempfile.TemporaryDirectory() as root:
        python = os.path.abspath(args.python)
        qualify(Path(root), args.library.resolve(), python)
        if args.micropython:
            micropython(
                Path(root), args.micropython.resolve(), args.modules.resolve(), python
            )
