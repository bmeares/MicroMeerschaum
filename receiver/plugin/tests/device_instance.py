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
        self.allow_actions_op = True
        self.allow_catalog_op = True
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
                    if not self.allow_catalog_op:
                        failure = "operation unavailable"
                    else:
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
                elif req["op"] == "actions":
                    if not self.allow_actions_op:
                        failure = "operation unavailable"
                    else:
                        data = ["show pipes", "help"]
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
                    # Simulate a v0.2.1 device without additive sync_time/rowcount ops.
                    failure = "operation unavailable"
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
    env.pop("NO_COLOR", None)
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


def _make_counting_python(root, real_python, log_path):
    wrapper = root / "counting_python.py"
    wrapper.write_text(
        f"#!{real_python}\n"
        "import os, sys\n"
        f"with open({str(log_path)!r}, 'a') as f:\n"
        "    f.write(f'{os.getpid()}\\n')\n"
        f"os.execv({real_python!r}, [{real_python!r}, *sys.argv[1:]])\n"
    )
    wrapper.chmod(0o755)
    return str(wrapper)


def shell(device):
    root = Path(os.environ["MRSM_ROOT_DIR"])
    base_cfg = json.loads(os.environ["MRSM_CONFIG"])
    real_python = base_cfg["meerschaum"]["connectors"]["micro"]["bench"]["python"]
    spawn_log = root / "transport_spawns.log"
    counting_python = _make_counting_python(root, real_python, spawn_log)

    def count_spawns():
        if not spawn_log.exists():
            return 0
        return len(spawn_log.read_text().splitlines())

    def counted_env():
        cfg = json.loads(os.environ["MRSM_CONFIG"])
        cfg["meerschaum"]["connectors"]["micro"]["bench"]["python"] = counting_python
        return {"MRSM_CONFIG": json.dumps(cfg)}

    # 1. Interactive styled TTY session: persistent transport reuse across completions & commands,
    #    contextual flag/key Tab completion, banner, toolbar, icons, conveniences, disconnect safety,
    #    reconnect on next command, history persistence, and clean close message.
    before_actions = list(device.actions)
    spawn_log.unlink(missing_ok=True)
    master, proc, until, wait_prompt, drain_exit, _ = _open_shell_pty(extra_env=counted_env())
    try:
        frame = wait_prompt()
        assert b"serial" in frame and b"connected" in frame, frame
        assert count_spawns() == 1, count_spawns()

        os.write(master, b"abandon\x03")
        wait_prompt()

        # Action completion + execution over the same persistent session.
        os.write(master, b"sh\t")
        until(b"show pipes")
        os.write(master, b"\r")
        until(b"Ran show pipes")
        frame = wait_prompt()
        assert b"serial" in frame and b"connected" in frame, frame
        assert count_spawns() == 1, count_spawns()

        # Contextual Tab completion: long selector flag, -m metric key, -l location key,
        # -c connector key, and -i instance key — all from cache with ZERO reconnects.
        os.write(master, b"show pipes --me\t")
        until(b"--metric-keys")
        os.write(master, b" readi\t")
        until(b"readings")
        os.write(master, b" -l o\t")
        until(b"outside")
        os.write(master, b" -c s\t")
        until(b"sensor")
        os.write(master, b" -i j\t")
        until(b"journal:local")
        os.write(master, b"\r")
        until(b"Ran show pipes --metric-keys readings -l outside -c sensor -i journal:local")
        wait_prompt()
        assert count_spawns() == 1, count_spawns()

        # Null location key completes as None.
        os.write(master, b"show pipes -l N\t")
        until(b"None")
        os.write(master, b"\r")
        until(b"Ran show pipes -l None")
        wait_prompt()
        assert count_spawns() == 1, count_spawns()

        # Flag menu after `-` suggests selector flags without spawning a transport.
        os.write(master, b"show pipes -\t")
        until(b"-c")
        os.write(master, b"\x03")
        wait_prompt()
        assert count_spawns() == 1, count_spawns()

        # Explicit device error uses styled failure icon and keeps the persistent session alive.
        os.write(master, b"refuse\r")
        until(b"Device refused this action")
        frame = wait_prompt()
        assert b"serial" in frame and b"connected" in frame, frame
        assert count_spawns() == 1, count_spawns()

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
        assert count_spawns() == 1, count_spawns()

        # Mid-session link drop on an in-flight action: reports unknown outcome, marks toolbar
        # disconnected, NEVER replays the interrupted action, and does NOT auto-reconnect.
        disconnects_before = device.actions.count("disconnect")
        os.write(master, b"disconnect\r")
        until(b"Outcome unknown after sending action")
        frame = wait_prompt()
        assert b"serial" in frame and b"disconnected" in frame, frame
        assert device.actions.count("disconnect") == disconnects_before + 1, device.actions
        assert count_spawns() == 1, count_spawns()

        # Next user-initiated command lazily reconnects a fresh persistent session.
        os.write(master, b"show pipes\r")
        until(b"Ran show pipes")
        frame = wait_prompt()
        assert b"serial" in frame and b"connected" in frame, frame
        assert count_spawns() == 2, count_spawns()

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

    # Confirm local commands (clear, ?, sh?) were not sent as device actions and disconnect ran once.
    assert device.actions[len(before_actions) :] == [
        "show pipes",
        "show pipes --metric-keys readings -l outside -c sensor -i journal:local",
        "show pipes -l None",
        "refuse",
        "help show pipes",
        "help",
        "show pipes",
        "disconnect",
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
        # Up 4 times skips "quit", "show pipes", "disconnect" and recalls "mrsm show pipes".
        os.write(master, b"\x1b[A\x1b[A\x1b[A\x1b[A")
        until(b"mrsm show pipes")
        os.write(master, b"\r")
        until(b"Ran show pipes")
        wait_prompt()
        # Ctrl-R searches persistent history for "--help" -> "show pipes --help".
        os.write(master, b"\x12show pipes --h")
        # Reedline may highlight the matching prefix with SGR codes.
        history_match = re.sub(rb"\x1b\[[0-9;]*m", b"", until(b"elp"))
        assert b"show pipes --help" in history_match, history_match
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
        assert b" ] > " in frame and b"serial" in frame and b"connected" in frame, frame
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

    # 3c. Graceful fallback when device session does not grant `actions` or `catalog`.
    device.allow_actions_op = False
    device.allow_catalog_op = False
    spawn_log.unlink(missing_ok=True)
    try:
        master, proc, until, wait_prompt, drain_exit, _ = _open_shell_pty(extra_env=counted_env())
        try:
            frame = wait_prompt()
            assert b"serial" in frame and b"connected" in frame, frame
            os.write(master, b"sh\t")
            until(b"show pipes")
            os.write(master, b" --me\t")
            until(b"--metric-keys")
            os.write(master, b"\r")
            until(b"Ran show pipes --metric-keys")
            wait_prompt()
            assert count_spawns() == 1, count_spawns()
            os.write(master, b"quit\r")
            drain_exit()
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
    finally:
        device.allow_actions_op = True
        device.allow_catalog_op = True

    # 4. Piped (non-TTY) session: plain output, no banner, no close message, no toolbar, no ANSI,
    #    reusing one persistent transport session across all piped commands.
    piped_before = len(device.actions)
    spawn_log.unlink(missing_ok=True)
    piped_env = dict(os.environ, **counted_env())
    piped = subprocess.run(
        ["mrsm", "micro", "shell", "-i", "micro:bench"],
        cwd=os.environ["MRSM_ROOT_DIR"],
        input="show pipes\nclear\nsh?\nshow pipes --help\nmrsm show pipes\nexit\n",
        capture_output=True,
        text=True,
        env=piped_env,
        timeout=40,
    )
    assert piped.returncode == 0, (piped.stdout, piped.stderr)
    assert count_spawns() == 1, count_spawns()
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
        # Older v0.2.1 device fallback: missing columns/dtypes and operation unavailable.
        assert remote.get_columns_types() == {}
        assert remote.get_sync_time() is None
        assert remote.get_rowcount() == len(device.rows)
        rc_report = command("show", "rowcounts", "-i", "micro:bench", "-m", "readings")
        assert str(len(device.rows)) in rc_report, rc_report
        cols_report = command("show", "columns", "-i", "micro:bench", "-m", "readings")
        assert "readings" in cols_report, cols_report
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

    # Persistent streaming session mode: ready handshake, multi-request reuse, DeviceError keeps
    # session open, and timeout after sending an action reports unknown outcome and exits cleanly.
    session = subprocess.Popen(
        [python, str(PLUGIN / "src/transport.py")],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )
    try:
        session.stdin.write(json.dumps({"mode": "session", "config": settings}) + "\n")
        session.stdin.flush()
        ready = json.loads(session.stdout.readline())
        assert ready == {"ok": True, "ready": True}, ready

        session.stdin.write(json.dumps({"op": "actions"}) + "\n")
        session.stdin.flush()
        actions_reply = json.loads(session.stdout.readline())
        assert actions_reply == {"ok": True, "data": ["show pipes", "help"]}, actions_reply

        session.stdin.write(json.dumps({"op": "action", "line": "refuse"}) + "\n")
        session.stdin.flush()
        refuse_reply = json.loads(session.stdout.readline())
        assert not refuse_reply["ok"] and not refuse_reply.get("fatal"), refuse_reply

        # Session stays alive after an explicit DeviceError.
        session.stdin.write(json.dumps({"op": "action", "line": "show pipes"}) + "\n")
        session.stdin.flush()
        ok_reply = json.loads(session.stdout.readline())
        assert ok_reply == {"ok": True, "data": "Ran show pipes"}, ok_reply

        # Dropping the reply to an in-flight action reports unknown outcome and closes cleanly.
        session.stdin.write(json.dumps({"op": "action", "line": "disconnect"}) + "\n")
        session.stdin.flush()
        drop_reply = json.loads(session.stdout.readline())
        assert (
            not drop_reply["ok"]
            and drop_reply.get("fatal") is True
            and "Outcome unknown after sending action" in drop_reply["error"]
        ), drop_reply
        assert session.wait(10) == 0
    finally:
        if session.poll() is None:
            session.kill()
            session.wait()


def device_rows():
    from datetime import datetime, timezone
    import meerschaum as mrsm

    remote = mrsm.Pipe("sensor", "temperature", instance="micro:device")
    assert remote.columns == {"datetime": "tick"}, remote.columns
    assert remote.get_columns_types() == {"tick": "int", "value": "int"}, (
        remote.get_columns_types()
    )
    assert set(remote.dtypes.keys()) == {"tick", "value"}, remote.dtypes
    assert remote.get_sync_time() is None
    assert remote.get_rowcount() == 0

    source = mrsm.Pipe(
        "sensor",
        "temperature",
        instance="sql:main",
        columns={"datetime": "tick"},
        dtypes={"tick": "int", "value": "int"},
    )
    assert source.sync([{"tick": 15, "value": 43}, {"tick": 2000, "value": 50}])[0]
    ok, message = remote.sync(source.get_docs(limit=2))
    assert ok, message
    # Mutate tick=15 on device and tick=2000 on device; incremental copy from sync_time=2000
    # with default 1440 backtrack window starts at tick=560, leaving tick=15 untouched.
    ok, message = remote.sync([{"tick": 15, "value": 99}, {"tick": 2000, "value": 55}])
    assert ok, message
    assert remote.get_sync_time() == 2000, remote.get_sync_time()
    assert remote.get_sync_time(params={"value": 99}) == 15
    assert remote.get_sync_time(params={"value": 999}) is None
    assert remote.get_rowcount() == 2
    assert remote.get_rowcount(begin=1998, end=2002) == 1
    assert remote.get_rowcount(params={"value": 55}) == 1
    filtered = remote.get_docs(begin=1998, end=2002, params={"value": 55}, limit=1)
    assert filtered == [{"tick": 2000, "value": 55}], filtered
    assert remote.get_docs(order="desc", limit=2) == [
        {"tick": 2000, "value": 55},
        {"tick": 15, "value": 99},
    ]
    local = mrsm.Pipe("station", "pull", instance="sql:main")
    assert local.sync(remote.get_docs(limit=2))[0]
    assert local.get_docs(limit=2) == [
        {"tick": 15, "value": 99},
        {"tick": 2000, "value": 55},
    ]

    # Datetime-axis sql:local pipe with extra key and mixed dtypes.
    weather = mrsm.Pipe("sensor", "weather", instance="micro:device")
    assert weather.columns == {"datetime": "dt", "station": "station"}, weather.columns
    assert weather.get_columns_types() == {
        "dt": "datetime",
        "station": "string",
        "celsius": "float",
        "ok": "bool",
    }, weather.get_columns_types()
    assert set(weather.dtypes.keys()) == {"dt", "station", "celsius", "ok"}, weather.dtypes
    t1 = datetime(2024, 6, 3, 16, 5, 1, 123456, tzinfo=timezone.utc)
    t2 = datetime(2024, 6, 3, 16, 10, 0, 0, tzinfo=timezone.utc)
    ok, message = weather.sync(
        [
            {"dt": t1, "station": "north", "celsius": 21.5, "ok": True},
            {"dt": t2, "station": "south", "celsius": 24.0, "ok": False},
        ]
    )
    assert ok, message
    assert weather.get_sync_time() == t2, weather.get_sync_time()
    assert weather.get_sync_time(params={"station": "north"}) == t1
    assert weather.get_rowcount() == 2
    assert weather.get_rowcount(params={"station": "south"}) == 1
    assert weather.get_docs(order="desc", limit=1) == [
        {"dt": t2, "station": "south", "celsius": 24.0, "ok": False}
    ]

    # sql:local pipe exceeding 128 rows: rowcount, sync_time, and descending tail reads.
    bulk = mrsm.Pipe("sensor", "bulk", instance="micro:device")
    assert bulk.get_rowcount() == 140
    assert bulk.get_rowcount(begin=100, end=120) == 20
    assert bulk.get_sync_time() == 140
    assert bulk.get_docs(order="desc", limit=3) == [
        {"tick": 140, "value": 1400},
        {"tick": 139, "value": 1390},
        {"tick": 138, "value": 1380},
    ]

    # Journal pipe: no schema/sync_time, exact rowcount, descending read rejected.
    journal = mrsm.Pipe("sensor", "audit", instance="micro:device")
    assert journal.get_columns_types() == {}
    assert journal.get_sync_time() is None
    assert journal.get_rowcount() == 0
    ok, message = journal.sync([{"when": t1, "event": "sample"}])
    assert ok, message
    assert journal.get_rowcount() == 1
    assert journal.get_docs(limit=1) == [{"when": t1, "event": "sample"}]
    try:
        journal.get_docs(limit=1, order="desc")
        raise AssertionError("journal accepted descending read")
    except Exception as exc:
        assert "ascending" in str(exc).lower(), exc

    # Add newer rows in sql:main for the incremental copy test below.
    assert source.sync([{"tick": 2005, "value": 60}, {"tick": 2010, "value": 65}])[0]


def verify_incremental_device_rows():
    import meerschaum as mrsm

    remote = mrsm.Pipe("sensor", "temperature", instance="micro:device")
    assert remote.get_rowcount() == 4, remote.get_rowcount()
    assert remote.get_sync_time() == 2010, remote.get_sync_time()
    assert remote.get_docs(limit=4) == [
        {"tick": 15, "value": 99},
        {"tick": 2000, "value": 50},
        {"tick": 2005, "value": 60},
        {"tick": 2010, "value": 65},
    ], remote.get_docs(limit=4)
    copied = mrsm.Pipe("sensor", "temperature", instance="sql:local")
    assert copied.get_rowcount() == 4, copied.get_rowcount()
    assert copied.get_docs(limit=4) == remote.get_docs(limit=4)


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
        detail = command("show", "pipes", "-i", "micro:device", "-m", "weather")
        assert "weather" in detail, detail
        detail_json = json.loads(
            command("show", "pipes", "-i", "micro:device", "-m", "weather", "--nopretty")
        )
        assert detail_json["parameters"]["columns"] == {
            "datetime": "dt",
            "station": "station",
        }, detail_json
        assert detail_json["parameters"]["dtypes"] == {
            "dt": "datetime",
            "station": "string",
            "celsius": "float",
            "ok": "bool",
        }, detail_json
        cols = command("show", "columns", "-i", "micro:device", "-m", "weather")
        assert (
            "dt" in cols
            and "station" in cols
            and "celsius" in cols
            and "datetime" in cols
        ), cols
        counts = command("show", "rowcounts", "-i", "micro:device")
        assert "bulk" in counts and "140" in counts and "weather" in counts, counts
        bulk_data = command("show", "data", "-i", "micro:device", "-m", "bulk")
        assert "140" in bulk_data and "1400" in bulk_data, bulk_data
        weather_data = command("show", "data", "-i", "micro:device", "-m", "weather")
        assert "south" in weather_data and "21.5" in weather_data, weather_data
        # Incremental copy into an ALREADY-POPULATED writable sql:local pipe on the device!
        inc_report = command(
            "copy",
            "pipes",
            "micro:device",
            "-i",
            "sql:main",
            "-m",
            "temperature",
            "--sync-data",
            "incremental",
            "-y",
        )
        assert "Failed" not in inc_report, inc_report
        # Reverse copy (device -> sql:local) with columns set must copy all rows, not just newest.
        rev_report = command(
            "copy",
            "pipes",
            "sql:local",
            "-i",
            "micro:device",
            "-m",
            "temperature",
            "--sync-data",
            "all",
            "-y",
        )
        assert "Failed" not in rev_report, rev_report
        subprocess.run(
            [
                sys.executable,
                "-c",
                "from device_instance import verify_incremental_device_rows; verify_incremental_device_rows()",
            ],
            check=True,
            cwd=PLUGIN / "tests",
            timeout=60,
        )
        print(
            "Passed host SQL <-> device SQLite schema, rowcount (>128), sync_time, show data/columns/rowcounts, and incremental copy."
        )
    finally:
        proc.terminate()
        proc.wait(5)
        os.close(slave)
        os.close(master)


def auto_detect_tests(root, library):
    """Verify zero-config USB auto-detection, direct port labels, multi-port probing, and stdlib POSIX serial."""
    plugins = root / "plugins"
    config = {
        "meerschaum": {
            "instance": "sql:main",
            "executor": "local",
            "connectors": {
                "sql": {
                    "main": {"flavor": "sqlite", "database": str(root / "auto.db")}
                }
            },
        }
    }
    base_env = dict(
        os.environ,
        MRSM_ROOT_DIR=str(root),
        MRSM_PLUGINS_DIR=str(plugins),
        MRSM_NO_AUTO_INSTALL="1",
        MRSM_NO_UPDATE_CHECK="1",
        MRSM_CONFIG=json.dumps(config),
    )

    def run_cmd(*argv, env_extra=None, stdin_text=None, ok=True):
        env = dict(base_env, **(env_extra or {}))
        result = subprocess.run(
            ["mrsm", *argv],
            cwd=str(root),
            env=env,
            input=stdin_text.encode() if stdin_text is not None else None,
            capture_output=True,
            timeout=30,
        )
        out = (result.stdout + result.stderr).decode()
        assert (result.returncode == 0) == ok, (argv, result.returncode, out)
        return out

    # 1. Zero USB ports -> clear actionable error.
    err = run_cmd("micro", "action", "show pipes", env_extra={"MRSM_MICRO_USB_PORTS": ""}, ok=False)
    assert "No USB MicroMeerschaum device detected" in err, err

    # 2. Single USB port -> works zero-config with stdlib POSIX serial (no pyserial needed).
    dev1 = Device()
    try:
        env1 = {"MRSM_MICRO_USB_PORTS": dev1.port, "MRSM_PYTHON": sys.executable}
        out = run_cmd("micro", "action", "show pipes", env_extra=env1)
        assert "Ran show pipes" in out, out
        piped = run_cmd("micro", "shell", env_extra=env1, stdin_text="show pipes\nquit\n")
        assert "Ran show pipes" in piped, piped
        pipes_auto = run_cmd("show", "pipes", "-i", "micro:auto", env_extra=env1)
        assert "readings" in pipes_auto and "readonly" in pipes_auto, pipes_auto
        pipes_direct = run_cmd(
            "show", "pipes", "-i", f"micro:{dev1.port}", env_extra={"MRSM_MICRO_USB_PORTS": "", "MRSM_PYTHON": sys.executable}
        )
        assert "readings" in pipes_direct, pipes_direct

        # 3. Multiple ports (one silent non-micro port + one live MicroMeerschaum device) -> probes and selects live device.
        silent_master, silent_slave = pty.openpty()
        tty.setraw(silent_slave)
        silent_port = os.ttyname(silent_slave)
        try:
            env_mixed = {
                "MRSM_MICRO_USB_PORTS": f"{silent_port}{os.pathsep}{dev1.port}",
                "MRSM_PYTHON": sys.executable,
            }
            out = run_cmd("micro", "action", "show pipes", env_extra=env_mixed)
            assert "Ran show pipes" in out, out
        finally:
            os.close(silent_master)
            os.close(silent_slave)

        # 4. Two live MicroMeerschaum devices -> returns clear disambiguation error listing both ports.
        dev2 = Device()
        try:
            env_both = {
                "MRSM_MICRO_USB_PORTS": f"{dev1.port}{os.pathsep}{dev2.port}",
                "MRSM_PYTHON": sys.executable,
            }
            err_both = run_cmd("micro", "action", "show pipes", env_extra=env_both, ok=False)
            assert "Multiple MicroMeerschaum USB devices detected" in err_both, err_both
            assert dev1.port in err_both and dev2.port in err_both, err_both
        finally:
            dev2.close()
    finally:
        dev1.close()
    print("Passed zero-config USB auto-detection, direct port labels, multi-port probing, and stdlib POSIX serial.")


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
        auto_detect_tests(Path(root), args.library.resolve())
        if args.micropython:
            micropython(
                Path(root), args.micropython.resolve(), args.modules.resolve(), python
            )
