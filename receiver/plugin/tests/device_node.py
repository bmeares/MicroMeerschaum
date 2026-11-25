# SPDX-License-Identifier: Apache-2.0
"""Unix MicroPython fixture for the deployed host plugin."""

import sys
from micromeerschaum import expose, provision
from mm_session import DeviceSession, serve_usb

path = sys.argv[1]
pipe = provision(
    "sensor",
    "temperature",
    path + "/device.db",
    1,
    1,
    2,
    instance="sql:local",
    axis="tick",
    create=True,
    columns=(("tick", "int", True), ("value", "int", False)),
)
expose(pipe, read=True, write=True)
journal = provision("sensor", "audit", path + "/audit.mm", 1, 2, 2, create=True)
expose(journal, read=True, write=True)
session = DeviceSession(
    operations=("action", "complete", "read", "write", "catalog"),
    allowed_actions=("show pipes",),
)
with open(path + "/device.ready", "w") as ready:
    ready.write("ready")
serve_usb(session)
