"""Exercise first-use commissioning at the aioble event boundary on Unix."""
import asyncio
import sys
from offline_relay import identity_address, enroll_receiver, enroll_sender

class Radio:
    handlers = []
    replies = []
    settings = {}
    def config(self, key=None, **kw):
        self.settings.update(kw)
        if key == "mac":
            assert self.settings["addr_mode"] == 0
            return (0, b"abcdef")

    def gap_passkey(self, handle, action, value):
        self.replies.append((handle, action, value))

    def register_irq_handler(self, irq, shutdown):
        self.handlers.append(irq)

    def event(self, handle, action):
        for irq in self.handlers:
            irq(31, (handle, action, 0))

    async def advertise(self, interval_us, **kw):
        assert self.settings["bond"] and self.settings["mitm"] and self.settings["le_secure"]
        assert self.settings["io"] == 0
        self.event(42, 3)  # Another connected, uncommissioned device.
        self.event(41, 2)  # Unexpected action.
        self.event(41, 3)
        conn.encrypted = conn.authenticated = conn.bonded = True
        return conn

class Connection:
    _connected = {}
    _conn_handle = 41
    encrypted = authenticated = bonded = False
    def __init__(self, device):
        self.device = device

    def is_connected(self):
        return True

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exception):
        pass

    class timeout:
        def __init__(self, timeout_ms):
            pass
        def __enter__(self):
            pass
        def __exit__(self, *exception):
            pass

    async def disconnected(self, timeout_ms):
        pass

    async def pair(self, **kw):
        assert kw["io"] == 2 and kw["le_secure"] and kw["mitm"] and kw["bond"]
        radio.event(42, 2)
        radio.event(41, 2)

class Peer:
    async def connect(self):
        return conn

radio = Radio()
radio.ble = radio
radio.DeviceConnection = Connection
sys.modules["aioble"] = radio
sys.modules["aioble.core"] = radio
sys.modules["aioble.device"] = radio
peer = Peer()
conn = Connection(peer)
Connection._connected.update({41: conn, 42: Connection(Peer())})

assert identity_address() == (0, b"abcdef")
asyncio.run(enroll_receiver(peer, "083457"))
radio.event(41, 3)
assert radio.replies == [(41, 3, 83457)]
asyncio.run(enroll_sender(peer, "094568"))
radio.event(41, 2)
assert radio.replies == [(41, 3, 83457), (41, 2, 94568)]
assert radio.settings["io"] == 3
# A failed pairing must close the passkey window as well.
conn.authenticated = False
try:
    asyncio.run(enroll_sender(peer, "123450"))
    raise AssertionError("accepted an unauthenticated link")
except OSError:
    pass
n = len(radio.replies)
radio.event(41, 2)
assert len(radio.replies) == n and radio.settings["io"] == 3
