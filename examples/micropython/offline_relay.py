# SPDX-License-Identifier: Apache-2.0
"""A captures, B takes custody, D archives. Each board opens only its own role."""
from micromeerschaum import provision
from mm_ble import Peripheral, deliver


def open_role(role, path, create=False):
    if role not in ("origin", "relay", "destination"):
        raise ValueError("unknown role")
    return provision("sensor", "temperature", path, origin=1, generation=5, destination=9,
                     custodian=None if role == "destination" else 2,
                     receiver=role != "origin", create=create)


def identity_address():
    """Read this ESP32-S3's fixed public identity address at its own console."""
    import aioble
    aioble.config(addr_mode=0, bond=True, le_secure=True, mitm=True, io=3)
    return aioble.config("mac")


# The pinned aioble has one shared IRQ dispatcher and no unregister operation.
# Leave one inert handler installed between explicit commissioning windows.
_enrollment = None
_registered = False
_INPUT = 2
_DISPLAY = 3


def _passkey_irq(event, data):
    if _enrollment is None:
        return
    if event == 31:  # _IRQ_PASSKEY_ACTION
        _enrollment.respond(*data)
    elif event in (2, 8) and _enrollment.pending == data[0]:
        # A later connection may reuse this handle.
        _enrollment.pending = None


class _Enrollment:
    def __init__(self, device, code, action):
        if not isinstance(code, str) or len(code) != 6 or any(c not in "0123456789" for c in code):
            raise ValueError("enter a fresh six-digit passkey")
        self.device = device
        self.code = int(code)
        self.action = action
        self.connection = None
        self.pending = None
        self.io = 0 if action == _DISPLAY else 2  # DisplayOnly or KeyboardOnly.

    def __enter__(self):
        global _enrollment, _registered
        import aioble
        from aioble.core import register_irq_handler
        if _enrollment is not None:
            raise ValueError("enrollment already in progress")
        if not _registered:
            register_irq_handler(_passkey_irq, None)
            _registered = True
        aioble.config(addr_mode=0, bond=True, le_secure=True, mitm=True, io=self.io)
        _enrollment = self
        return self

    def respond(self, handle, action, _number):
        from aioble.core import ble
        from aioble.device import DeviceConnection
        if action != self.action or self.connection is not None:
            return
        conn = DeviceConnection._connected.get(handle)
        if conn is None:
            # NimBLE can request the passkey before aioble receives CONNECT.
            # Defer supplying it until advertise/connect returns the verified peer.
            self.pending = handle
        elif conn.is_connected() and conn.device == self.device:
            self.pending = None
            self.connection = conn
            ble.gap_passkey(handle, action, self.code)

    def connected(self, conn):
        if conn._conn_handle is not None and self.pending == conn._conn_handle:
            self.pending = None
            self.respond(conn._conn_handle, self.action, None)

    def verify(self, conn):
        if self.connection is not conn or not (conn.encrypted and conn.authenticated and conn.bonded):
            raise OSError("passkey-authenticated bond required")

    def __exit__(self, *exception):
        global _enrollment
        import aioble
        _enrollment = None
        self.code = self.connection = self.pending = None
        aioble.config(bond=True, le_secure=True, mitm=True, io=3)


async def enroll_receiver(device, code):
    """Advertise for one expected central using the operator's fresh passkey."""
    import aioble
    import asyncio
    with _Enrollment(device, code, _DISPLAY) as enrollment:
        async with await aioble.advertise(100000, name=b"MM-enroll", timeout_ms=60000) as conn:
            if conn.device != device:
                raise OSError("unexpected commissioning peer")
            enrollment.connected(conn)
            with conn.timeout(30000):
                while not conn.encrypted:
                    await asyncio.sleep_ms(20)
            enrollment.verify(conn)


async def enroll_sender(device, code):
    """Connect to one expected peripheral and enter its out-of-band passkey."""
    with _Enrollment(device, code, _INPUT) as enrollment:
        async with await device.connect() as conn:
            enrollment.connected(conn)
            await conn.pair(bond=True, le_secure=True, mitm=True, io=enrollment.io)
            enrollment.verify(conn)
            await conn.disconnected(timeout_ms=30000)


def enrolled(device, node):
    # Fixed-address, previously enrolled, authenticated BLE bond on this board.
    def resolve(connection):
        if connection.bonded and connection.device == device:
            return node
    return resolve


async def receive_window(pipe, device, sender):
    identity_address()
    peripheral = Peripheral(pipe, enrolled(device, sender))
    try:
        await peripheral.serve_once()
    finally:
        peripheral.close()


async def send_window(pipe, device, issuer):
    identity_address()
    async with await device.connect() as connection:
        if not connection.encrypted:
            await connection.pair(bond=True, mitm=True, le_secure=True)
        authorize = enrolled(device, issuer)
        through = pipe.progress()["confirmed"]
        for _ in range(4):
            if through >= pipe.progress()["prefix"]:
                break
            next_through = await deliver(pipe, connection, authorize)
            if next_through <= through:
                raise OSError("receipt did not advance")
            through = next_through
        pipe.reclaim()
        return through
