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


class WriteGuard:
    def instruction(self, reason, token):
        return (
            reason
            + "\nAfter inspecting this Pipe, run mrsm micro reconcile "
            + token
            + " -i micro:"
            + self.label
            + " to allow another write."
        )

    def __init__(self, config, key, request_id):
        import sqlite3

        label = config.get("_micro_label")
        self.label = (
            label
            if isinstance(label, str) and re.fullmatch(r"[a-zA-Z0-9_.-]+", label)
            else "<label>"
        )
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
                raise RuntimeError(self.instruction(existing[1], existing[0]))
            reason = (
                "Outcome unknown for the previous write to " + json.dumps(key) + "."
            )
            self.db.execute("INSERT INTO pending VALUES (?, ?)", (self.token, reason))
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
        return self.instruction(reason, self.token)

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
    path = _directory(config) / (token.split(".")[0] + ".sqlite3")
    if not path.is_file():
        raise ValueError("No matching unresolved write for this device")
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
