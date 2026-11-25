"""Qualify the installed micro plugin with unmodified Meerschaum CLI and Pipe APIs on Linux."""

import argparse
import json
import os
import pty
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


class Device:
    def __init__(self):
        self.fd, self.slave = pty.openpty()
        tty.setraw(self.slave)
        self.port = os.ttyname(self.slave)
        self.rows = []
        self.writes = 0
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
                            write=i == 0,
                        )
                        for i, key in enumerate((DEFAULT_KEY, READ_ONLY))
                    ]
                    offset = req.get("offset", 0)
                    # Exercise page continuation even when the host requested more entries.
                    data = dict(items=items[offset : offset + 1], more=offset == 0)
                elif req["op"] == "write":
                    self.writes += 1
                    assert req["key"] == DEFAULT_KEY
                    self.rows.extend(req["rows"])
                    if req["rows"][0].get("simulate") == "drop":
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


def shell():
    master, slave = pty.openpty()
    proc = subprocess.Popen(
        ["mrsm", "micro", "shell", "-i", "micro:bench"],
        stdin=slave,
        stdout=slave,
        stderr=slave,
    )
    os.close(slave)
    transcript = b""

    def until(expected):
        nonlocal transcript
        end = time.monotonic() + 12
        while expected not in transcript:
            ready = select.select([master], [], [], max(0, end - time.monotonic()))[0]
            assert ready, transcript
            transcript += os.read(master, 8192)

    try:
        until(b"micro:bench> ")
        os.write(master, b"sh\t\n")
        until(b"Ran show pipes")
        os.write(master, b"exit\n")
        assert proc.wait(12) == 0
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)


def qualify(root, library, transport_python):
    plugins = root / "plugins"
    package = plugins / "micro"
    package.mkdir(parents=True)
    shutil.copy2(library, package / library.name)
    shutil.copy2(PLUGIN / "plugin.toml", package)
    device = Device()
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

        before = device.writes
        ok, msg = remote.sync([{"simulate": "drop"}])
        assert not ok and "unknown" in msg.lower() and device.writes == before + 1, msg
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
        shell()
        assert device.actions == [
            "show pipes",
            "show pipes -m readings",
            "refuse",
            "disconnect",
            "show pipes",
        ], device.actions
        print(
            "Passed native discovery, filtered listing, SQL transfers, bounded reads, permissions, timeout and terminal shell."
        )
    finally:
        device.close()


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
