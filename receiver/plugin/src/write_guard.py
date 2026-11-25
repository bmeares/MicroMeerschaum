# SPDX-License-Identifier: Apache-2.0
"""Keep one durable unresolved write per physical endpoint and published Pipe."""

import hashlib
import json
import os
import re
import sys
from pathlib import Path


def _digest(value):
    raw = json.dumps(value, ensure_ascii=True).encode()
    return hashlib.sha256(raw).hexdigest()[:32]


def _directory(config):
    transport = config.get("transport", "serial")
    if (
        transport in ("serial", "usb")
        and isinstance(config.get("port"), str)
        and config["port"]
    ):
        port = Path(config["port"])
        identity = os.path.normcase(os.path.realpath(port))
        if sys.platform.startswith("linux"):
            # by-id persists if Linux allocates a new tty number after reboot.
            devices = Path("/dev/serial/by-id")
            if port.parent == devices:
                identity = str(port)
            elif devices.is_dir():
                aliases = sorted(
                    link
                    for link in devices.iterdir()
                    if os.path.realpath(link) == identity
                )
                if aliases:
                    identity = str(aliases[0])
        endpoint = ["serial", identity]
    elif (
        transport == "ble"
        and isinstance(config.get("address"), str)
        and config["address"]
    ):
        endpoint = ["ble", config["address"].upper()]
    else:
        raise ValueError("Configure the device transport before writing")
    state = os.environ.get("XDG_STATE_HOME") or os.environ.get("LOCALAPPDATA")
    root = Path(state) if state else Path.home() / ".local" / "state"
    return root / "micromeerschaum" / "writes" / _digest(endpoint)


def _instruction(label, reason, token):
    if not isinstance(label, str) or not re.fullmatch(r"[a-zA-Z0-9_.-]+", label):
        label = "<label>"
    return (
        reason
        + "\nAfter inspecting this Pipe, run mrsm micro reconcile "
        + token
        + " -i micro:"
        + label
        + " to allow another write."
    )


def _port(config):
    if config.get("transport", "serial") in ("serial", "usb") and config.get("port"):
        return os.path.abspath(config["port"])
    return None


def _incident(config, pipe, token=None):
    """Find the durable incident even when unplugging removed a Linux by-id symlink."""
    import sqlite3

    directory = _directory(config)
    filename = pipe + ".sqlite3"
    port = _port(config)

    def candidates():
        yield directory / filename, False
        if sys.platform.startswith("linux") and port and not os.path.exists(port):
            for path in directory.parent.glob("*/" + filename):
                if path.parent != directory:
                    yield path, True

    for path, match_port in candidates():
        if not path.is_file():
            continue
        db = sqlite3.connect(path.absolute().as_uri() + "?mode=ro", uri=True)
        try:
            if match_port:
                if not db.execute(
                    "SELECT 1 FROM sqlite_master WHERE name='serial_port'"
                ).fetchone():
                    continue
                row = db.execute(
                    "SELECT token, reason FROM pending JOIN serial_port USING (token) WHERE path = ?",
                    (port,),
                ).fetchone()
            else:
                row = db.execute("SELECT token, reason FROM pending").fetchone()
            if row and (token is None or token == row[0]):
                return path, row[0], row[1]
        finally:
            db.close()
    return None


def pending_write(config, key, label):
    incident = _incident(config, _digest(key))
    if incident:
        _, token, reason = incident
        return _instruction(label, reason, token)
    return None


class WriteGuard:
    def instruction(self, reason):
        return _instruction(self.label, reason, self.token)

    def __init__(self, config, key, request_id):
        import sqlite3

        self.label = config.get("_micro_label")
        directory = _directory(config)
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        pipe = _digest(key)
        self.token = pipe + "." + request_id
        self.db = sqlite3.connect(directory / (pipe + ".sqlite3"), isolation_level=None)
        try:
            self.db.execute("PRAGMA synchronous=FULL")
            self.db.execute("BEGIN IMMEDIATE")
            self.db.execute(
                "CREATE TABLE IF NOT EXISTS pending (token TEXT PRIMARY KEY, reason TEXT NOT NULL)"
            )
            existing = self.db.execute("SELECT token, reason FROM pending").fetchone()
            if existing:
                raise RuntimeError(_instruction(self.label, existing[1], existing[0]))
            reason = (
                "Outcome unknown for the previous write to " + json.dumps(key) + "."
            )
            self.db.execute("INSERT INTO pending VALUES (?, ?)", (self.token, reason))
            port = _port(config)
            if (
                sys.platform.startswith("linux")
                and port
                and _digest(["serial", port]) != directory.name
            ):
                self.db.execute(
                    "CREATE TABLE IF NOT EXISTS serial_port (path TEXT, token TEXT)"
                )
                self.db.execute("DELETE FROM serial_port")
                self.db.execute(
                    "INSERT INTO serial_port VALUES (?, ?)", (port, self.token)
                )
            self.db.commit()
            # Hold the per-Pipe writer lock through transport and acknowledgement.
            # Check the token again after crossing the durability boundary.
            self.db.execute("BEGIN IMMEDIATE")
            row = self.db.execute("SELECT token FROM pending").fetchone()
            if row != (self.token,):
                raise RuntimeError("Write was reconciled before transmission")
        except BaseException:
            self.db.close()
            raise

    def failed(self, reason):
        self.db.execute(
            "UPDATE pending SET reason = ? WHERE token = ?", (reason, self.token)
        )
        self.db.commit()
        return self.instruction(reason)

    def clear(self):
        self.db.execute("DELETE FROM pending WHERE token = ?", (self.token,))
        self.db.commit()

    def close(self):
        self.db.close()


def reconcile(config, token):
    import sqlite3

    if not isinstance(token, str) or not re.fullmatch(
        r"[0-9a-f]{32}\.[0-9a-f]{16}", token
    ):
        raise ValueError("Provide the recovery token from the unresolved write")
    incident = _incident(config, token.split(".")[0], token)
    if not incident:
        raise ValueError("No matching unresolved write for this device")
    path = incident[0]
    db = sqlite3.connect(path, isolation_level=None)
    try:
        db.execute("BEGIN IMMEDIATE")
        deleted = db.execute("DELETE FROM pending WHERE token = ?", (token,))
        if not deleted.rowcount:
            raise ValueError("No matching unresolved write for this device")
        db.commit()
        return "Reconciliation recorded. The next write to this Pipe is permitted."
    finally:
        db.close()
