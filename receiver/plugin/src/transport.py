# SPDX-License-Identifier: Apache-2.0
"""One bounded device session request. Install pyserial for USB or bleak for Linux BLE."""
import asyncio
import json
import math
import secrets
import sys
import time

PREFIX = b"@micro "
MAX_FRAME = 4096
SERVICE = "2f725f70-9771-4f95-bb68-5e47ea680001"
RX = SERVICE[:-1] + "2"
TX = SERVICE[:-1] + "3"


class DeviceError(RuntimeError):
    """The device returned an explicit failure."""


class Response:
    def __init__(self, request_id):
        self.id = request_id
        self.line = bytearray()
        self.discard = False

    def feed(self, chunk):
        for octet in chunk:
            if octet != 10:
                if len(self.line) >= MAX_FRAME - 1:
                    self.discard = True
                elif not self.discard:
                    self.line.append(octet)
                continue
            frame = self.line
            discard = self.discard
            self.line = bytearray()
            self.discard = False
            if discard or not frame.startswith(PREFIX):
                continue
            try:
                obj = json.loads(frame[len(PREFIX):])
            except (ValueError, UnicodeError):
                continue
            if not isinstance(obj, dict) or obj.get("id") != self.id or obj.get("v") != 1:
                continue
            if obj.get("ok") is False:
                raise DeviceError(str(obj.get("error", "Device action failed")))
            if obj.get("ok") is True and "data" in obj:
                return True, obj["data"]
        return False, None


def _serial(config, frame, response, timeout, sending):
    import serial
    port = config.get("port")
    if not isinstance(port, str) or not port:
        raise ValueError("A USB serial port is required")
    link = serial.Serial(port=None, baudrate=int(config.get("baudrate", 115200)),
                         timeout=min(0.1, timeout), write_timeout=timeout)
    # Opening the control channel should leave the board running.
    link.dtr = False
    link.rts = False
    link.port = port
    if sys.platform.startswith("linux"):
        link.exclusive = True
    with link:
        deadline = time.monotonic() + timeout
        sending[0] = True
        if link.write(frame) != len(frame):
            raise OSError("Incomplete serial write")
        while time.monotonic() < deadline:
            chunk = link.read(max(1, min(link.in_waiting, MAX_FRAME)))
            done, data = response.feed(chunk)
            if done:
                return data
        raise TimeoutError("Timed out waiting for the device reply")


async def _bluetooth(config, frame, response, timeout, sending):
    from bleak import BleakClient, BleakScanner
    address = config.get("address")
    if not isinstance(address, str) or not address:
        raise ValueError("An enrolled BLE identity address is required")
    if not sys.platform.startswith("linux"):
        raise RuntimeError("The BLE host transport requires Linux BlueZ bond information")
    device = await BleakScanner.find_device_by_address(address, timeout=timeout)
    if device is None:
        raise OSError("The configured BLE peer is unavailable")
    properties = device.details.get("props", {})
    if not (properties.get("Paired") and properties.get("Bonded")):
        raise RuntimeError("Enroll the peer with authenticated Bluetooth pairing before use")
    queue = asyncio.Queue(maxsize=MAX_FRAME // 20 + 8)
    overflow = False

    def receive(_, data):
        nonlocal overflow
        if queue.full():
            overflow = True
        else:
            queue.put_nowait(bytes(data))

    def disconnected(_):
        receive(None, b"")

    async with BleakClient(device, disconnected_callback=disconnected, timeout=timeout) as client:
        service = client.services.get_service(SERVICE)
        if service is None:
            raise RuntimeError("The peer has no MicroMeerschaum action service")
        rx, tx = service.get_characteristic(RX), service.get_characteristic(TX)
        if rx is None or tx is None:
            raise RuntimeError("Incomplete MicroMeerschaum action service")
        await client.start_notify(tx, receive)
        for i in range(0, len(frame), 20):
            sending[0] = True
            await client.write_gatt_char(rx, frame[i:i+20], response=True)
        while True:
            chunk = await queue.get()
            if not chunk or overflow:
                raise OSError("Lost the BLE reply")
            done, data = response.feed(chunk)
            if done:
                return data


def exchange(config, request):
    """Execute one request. No write or action is automatically repeated."""
    op = request["op"]
    token = secrets.token_hex(8)
    envelope = dict(request, v=1, id=token)
    frame = PREFIX + json.dumps(envelope, allow_nan=False, ensure_ascii=True,
                                separators=(",", ":")).encode() + b"\n"
    if len(frame) > MAX_FRAME:
        raise ValueError("Device request exceeds 4096 bytes")
    timeout = float(config.get("timeout", 10))
    if not math.isfinite(timeout) or not 0 < timeout <= 300:
        raise ValueError("timeout must be in (0, 300] seconds")
    sending = [False]
    response = Response(token)
    try:
        transport = config.get("transport", "serial")
        if transport in ("usb", "serial"):
            return _serial(config, frame, response, timeout, sending)
        if transport == "ble":
            return asyncio.run(asyncio.wait_for(
                _bluetooth(config, frame, response, timeout, sending), timeout))
        raise ValueError("transport must be serial or ble")
    except DeviceError:
        raise
    except Exception as exc:
        detail = str(exc) or type(exc).__name__
        if sending[0] and op in ("write", "action"):
            raise RuntimeError("Outcome unknown after sending %s: %s" % (op, detail)) from exc
        raise RuntimeError(detail) from exc


if __name__ == "__main__":
    try:
        inputs = json.load(sys.stdin)
        result = {"ok": True, "data": exchange(inputs["config"], inputs["request"])}
    except Exception as failure:
        result = {"ok": False, "error": str(failure)}
    print(json.dumps(result, allow_nan=False, separators=(",", ":")))
