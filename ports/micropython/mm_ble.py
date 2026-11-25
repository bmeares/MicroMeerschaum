# SPDX-License-Identifier: Apache-2.0
"""MicroMeerschaum BLE exchange. Radio imports are deferred for Unix transports."""
import struct

SERVICE = "d9da0001-5af1-49e5-9d3a-825119b55722"
TX = "d9da0002-5af1-49e5-9d3a-825119b55722"
RX = "d9da0003-5af1-49e5-9d3a-825119b55722"
VALUE_LIMIT = 244
RECEIPT_LIMIT = 256  # MM_RECEIPT_FIXED_SIZE + MM_KEY_MAX
_HEADER = 9


def _value_limit(connection):
    return min(VALUE_LIMIT, (connection.mtu or 23) - 3)


class GattSession:
    """Peripheral fragment state for exactly one authorized connection."""
    def __init__(self, receive, batch_limit=4096):
        if batch_limit <= 0:
            raise ValueError("invalid batch budget")
        self.buffer = bytearray(batch_limit)
        self.receive = receive
        self.reset()

    def reset(self):
        self.total = self.at = 0
        self.reply = self.value = None

    def write(self, fragment, value_limit):
        try:
            if not _HEADER <= len(fragment) <= value_limit <= VALUE_LIMIT:
                raise ValueError("invalid characteristic length")
            op, at, total = struct.unpack("<BII", fragment[:_HEADER])
            if op == 2:
                if (len(fragment) != _HEADER or self.reply is None or
                    at >= len(self.reply) or total <= _HEADER):
                    raise ValueError("invalid receipt offset or size")
                end = at + min(value_limit, total) - _HEADER
                self.value = struct.pack("<BII", 3, at, len(self.reply)) + self.reply[at:end]
                return
            if op != 1 or len(fragment) == _HEADER:
                raise ValueError("invalid PUSH")
            if at == 0:
                self.reset()
                if not 0 < total <= len(self.buffer):
                    raise ValueError("batch exceeds budget")
                self.total = total
            end = at + len(fragment) - _HEADER
            if total != self.total or at != self.at or end > total:
                raise ValueError("inconsistent fragment")
            self.buffer[at:end] = memoryview(fragment)[_HEADER:]
            self.at = end
            if self.at == self.total:
                receipt = self.receive(memoryview(self.buffer)[:end])
                if not 0 < len(receipt) <= RECEIPT_LIMIT:
                    raise ValueError("invalid receipt size")
                self.reply = bytes(receipt)
                self.total = self.at = 0
        except Exception:
            self.reset()
            raise

    def read(self):
        if self.value is None:
            raise ValueError("receipt fragment unavailable")
        return self.value


async def exchange(write, read, batch, value_limit=20):
    """Send one batch using async write-with-response and read callables."""
    if not _HEADER < value_limit <= VALUE_LIMIT or not 0 < len(batch) <= 0xffffffff:
        raise ValueError("invalid transfer size")
    for at in range(0, len(batch), value_limit - _HEADER):
        fragment = batch[at:at + value_limit - _HEADER]
        await write(struct.pack("<BII", 1, at, len(batch)) + fragment)

    reply = bytearray()
    total = None
    while total is None or len(reply) < total:
        await write(struct.pack("<BII", 2, len(reply), value_limit))
        data = await read()
        if not _HEADER < len(data) <= value_limit:
            raise ValueError("invalid receipt fragment size")
        op, at, size = struct.unpack("<BII", data[:_HEADER])
        if (op != 3 or at != len(reply) or not 0 < size <= RECEIPT_LIMIT or
            (total is not None and size != total) or len(data) - _HEADER > size - at):
            raise ValueError("inconsistent receipt fragment")
        total = size
        reply.extend(memoryview(data)[_HEADER:])
    return bytes(reply)


def _identity(connection, authorize):
    if connection is None or not connection.is_connected() or not (
        connection.encrypted and connection.authenticated
    ):
        raise OSError("authenticated BLE link required")
    node = authorize(connection)
    if type(node) is not int or not 0 <= node < (1 << 64):
        raise OSError("peer is not authorized for this Pipe")
    return node


async def deliver(pipe, connection, authorize, max_records=8, cap=4096, timeout_ms=10000):
    """Deliver one journal batch on an authenticated aioble DeviceConnection.

    authorize(connection) resolves the enrolled link identity to a node number.
    Reconnect and retry the journal on disconnect, timeout or lost reply.
    """
    import bluetooth
    issuer = _identity(connection, authorize)
    service = await connection.service(bluetooth.UUID(SERVICE), timeout_ms=timeout_ms)
    if service is None:
        raise OSError("peer service unavailable")
    tx = await service.characteristic(bluetooth.UUID(TX), timeout_ms=timeout_ms)
    rx = await service.characteristic(bluetooth.UUID(RX), timeout_ms=timeout_ms)
    if tx is None or rx is None:
        raise OSError("peer characteristics unavailable")

    def check():
        if _identity(connection, authorize) != issuer:
            raise OSError("link identity changed")
    async def write(value):
        check()
        await tx.write(value, response=True, timeout_ms=timeout_ms)
    async def read():
        check()
        return await rx.read(timeout_ms=timeout_ms)

    receipt = await exchange(write, read, pipe.batch(max_records=max_records, cap=cap),
                             _value_limit(connection))
    check()
    pipe.confirm(issuer, receipt)
    return pipe.progress()["confirmed"]


# aioble dispatches write events synchronously on the ESP32 NimBLE port. Its
# capture coroutine can run after the following native ATT read. Consume the
# write in the synchronous callback and authorize each read before returning.
_peripheral = None
_irq_registered = False


def _write_irq(event, data):
    server = _peripheral
    if server is None:
        return
    if event == 3 and data[1] == server.tx._value_handle:
        server._written(data[0])
    elif event == 2 and server.connection is not None and data[0] == server.connection._conn_handle:
        server._reset()


class Peripheral:
    """One aioble peripheral connection at a time, with per-link fragment state."""
    def __init__(self, pipe, authorize, batch_limit=4096):
        global _peripheral, _irq_registered
        import aioble
        import bluetooth
        from aioble.core import register_irq_handler
        if _peripheral is not None:
            raise ValueError("BLE peer service already registered")
        self.pipe = pipe
        self.authorize = authorize
        self.connection = self.sender = None
        self.session = GattSession(self._receive, batch_limit)
        self.service = aioble.Service(bluetooth.UUID(SERVICE))
        self.tx = aioble.BufferedCharacteristic(self.service, bluetooth.UUID(TX), write=True,
                                                max_len=VALUE_LIMIT + 1)
        owner = self
        class Receipt(aioble.BufferedCharacteristic):
            def on_read(self, connection):
                try:
                    owner._check(connection)
                    self.write(owner.session.read())
                    return 0
                except Exception:
                    return 8  # ATT insufficient authorization / receipt unavailable.
        self.rx = Receipt(self.service, bluetooth.UUID(RX), read=True, max_len=VALUE_LIMIT)
        self.tx.flags |= 0x2000  # Bluetooth FLAG_WRITE_AUTHENTICATED.
        self.rx.flags |= 0x0400  # Bluetooth FLAG_READ_AUTHENTICATED.
        aioble.register_services(self.service)
        if not _irq_registered:
            register_irq_handler(_write_irq, None)
            _irq_registered = True
        _peripheral = self

    def _reset(self):
        self.session.reset()
        self.sender = None
        self.connection = None

    def _check(self, connection):
        if connection is None or connection is not self.connection:
            raise OSError("unknown connection")
        sender = _identity(connection, self.authorize)
        if self.sender is not None and sender != self.sender:
            self.session.reset()
            raise OSError("link identity changed")
        self.sender = sender

    def _receive(self, batch):
        self._check(self.connection)
        return self.pipe.receive(self.sender, batch)

    def _written(self, handle):
        connection = self.connection
        if connection is None or handle != connection._conn_handle:
            return
        try:
            self._check(connection)
            self.session.write(self.tx.read(), _value_limit(connection))
        except Exception:
            # MicroPython acknowledges the ATT write independently. Suppress
            # its receipt and make every subsequent read fail until a fresh PUSH.
            self.session.reset()

    async def serve_once(self, name="MicroMeerschaum", interval_us=250000):
        import aioble
        async with await aioble.advertise(interval_us, name=name, services=[self.service.uuid]) as conn:
            self.connection = conn
            try:
                await conn.disconnected()
            finally:
                self._reset()

    def close(self):
        global _peripheral
        self._reset()
        if _peripheral is self:
            _peripheral = None
