# SPDX-License-Identifier: Apache-2.0
"""Bonded management GATT service. Each connection starts a fresh device session."""
from mm_session import FrameReader

SERVICE = "2f725f70-9771-4f95-bb68-5e47ea680001"
RX = "2f725f70-9771-4f95-bb68-5e47ea680002"
TX = "2f725f70-9771-4f95-bb68-5e47ea680003"

_active = None
_installed = False


def _irq(event, data):
    server = _active
    if event not in (2, 3) or server is None or server.connection is None or data[0] != server.connection._conn_handle:
        return
    if event == 3 and data[1] == server.rx._value_handle:
        server._written()
    elif event == 2:
        server._reset()
        server.ready.set()


class ManagementPeripheral:
    """Serve one bonded, authenticated and explicitly allowlisted central at a time."""
    def __init__(self, session, peers):
        global _active, _installed
        import aioble
        import asyncio
        import bluetooth
        from aioble.core import register_irq_handler
        if _active is not None:
            raise ValueError("management peripheral already registered")
        self.peers = tuple(peers)
        self.session = session
        self.ready = asyncio.ThreadSafeFlag()
        self.service = aioble.Service(bluetooth.UUID(SERVICE))
        self.rx = aioble.BufferedCharacteristic(self.service, bluetooth.UUID(RX), write=True, max_len=513)
        self.rx.flags |= 0x2000  # FLAG_WRITE_AUTHENTICATED
        self.tx = aioble.Characteristic(self.service, bluetooth.UUID(TX), notify=True)
        self._reset()
        aioble.register_services(self.service)
        if not _installed:
            register_irq_handler(_irq, None)
            _installed = True
        _active = self

    def _reset(self):
        self.connection = self.pending = None
        self.frames = FrameReader()

    def authorized(self):
        c = self.connection
        return (c is not None and c.is_connected() and c.encrypted and c.authenticated and c.bonded
                and c.device in self.peers)

    def _queue(self, line):
        if self.pending is None:
            self.pending = line
            self.ready.set()

    def _written(self):
        # Consume write-with-response values synchronously. A finite aioble
        # capture queue may silently drop a middle fragment of a long request.
        if not self.authorized():
            self.frames = FrameReader()
            self.pending = None
            return
        fragment = self.rx.read()
        if len(fragment) > min(512, (self.connection.mtu or 23) - 3):
            self.frames.discard = True
            return
        self.frames.feed(fragment, self._queue)

    async def _respond(self, conn):
        import asyncio
        while self.connection is conn and conn.is_connected():
            await self.ready.wait()
            line, self.pending = self.pending, None
            if line is None or not self.authorized():
                continue
            reply = self.session.handle(line)
            if reply is None:
                continue
            mtu = min(512, (conn.mtu or 23) - 3)
            for at in range(0, len(reply), mtu):
                fragment = reply[at:at + mtu]
                if not await self._notify(conn, fragment):
                    return
                await asyncio.sleep_ms(1)

    async def _notify(self, conn, fragment):
        import asyncio
        import errno
        import time
        since = time.ticks_ms()
        while self.authorized():
            try:
                self.tx.notify(conn, fragment)
                return True
            except OSError as error:
                # Retry a locally refused enqueue of this fragment. The action
                # already ran once and its reply is held until sent or lost.
                if (error.args[0] not in (errno.EAGAIN, errno.ENOMEM, getattr(errno, "EBUSY", 16))
                        or time.ticks_diff(time.ticks_ms(), since) >= 5000):
                    raise
                await asyncio.sleep_ms(10)
        return False

    async def serve_once(self, name="MicroMeerschaum", interval_us=250000):
        import aioble
        import asyncio
        async with await aioble.advertise(interval_us, name=name, services=[self.service.uuid]) as conn:
            self.connection = conn
            if conn.device not in self.peers:
                self._reset()
                return
            worker = asyncio.create_task(self._respond(conn))
            try:
                await conn.disconnected()
            finally:
                self._reset()
                worker.cancel()
                try:
                    await worker
                except asyncio.CancelledError:
                    pass

    def close(self):
        global _active
        self._reset()
        if _active is self:
            _active = None
