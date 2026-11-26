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


class UnknownOutcome(RuntimeError):
    """A sent action or write may have executed."""


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


def _validate_timeout(config):
    timeout = float(config.get("timeout", 10))
    if not math.isfinite(timeout) or not 0 < timeout <= 300:
        raise ValueError("timeout must be in (0, 300] seconds")
    return timeout


def _build_frame(request, token=None):
    if not isinstance(request, dict) or not isinstance(request.get("op"), str) or not request["op"]:
        raise ValueError("Device request requires an operation name")
    op = request["op"]
    token = token or secrets.token_hex(8)
    envelope = dict(request, v=1, id=token)
    frame = PREFIX + json.dumps(envelope, allow_nan=False, ensure_ascii=True,
                                separators=(",", ":")).encode() + b"\n"
    if len(frame) > MAX_FRAME:
        raise ValueError("Device request exceeds 4096 bytes")
    return op, token, frame


def _serial_open(config, timeout):
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
    return link


def _serial_exchange(link, frame, response, timeout, sending):
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


def _serial(config, frame, response, timeout, sending):
    link = _serial_open(config, timeout)
    with link:
        return _serial_exchange(link, frame, response, timeout, sending)


async def _ble_resolve(config, timeout):
    from bleak import BleakScanner
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
    return device


async def _bluetooth(config, frame, response, timeout, sending):
    from bleak import BleakClient
    device = await _ble_resolve(config, timeout)
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


def exchange(config, request, token=None):
    """Execute one request. No write or action is automatically repeated."""
    op, token, frame = _build_frame(request, token)
    timeout = _validate_timeout(config)
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
            raise UnknownOutcome(f"Outcome unknown after sending {op}: {detail}") from exc
        raise RuntimeError(detail) from exc


def report(result):
    print(json.dumps(result, allow_nan=False, separators=(",", ":")), flush=True)


def _session_failure_message(op, sending, exc):
    detail = str(exc) or type(exc).__name__
    if sending[0] and op in ("write", "action"):
        return f"Outcome unknown after sending {op}: {detail}"
    return detail


def _serial_session(config, timeout):
    link = _serial_open(config, timeout)
    with link:
        report({"ok": True, "ready": True})
        while True:
            raw = sys.stdin.readline()
            if not raw:
                return
            raw = raw.strip()
            if not raw:
                continue
            try:
                request = json.loads(raw)
                op, token, frame = _build_frame(request)
            except Exception as exc:
                report({"ok": False, "error": str(exc) or type(exc).__name__})
                continue
            sending = [False]
            response = Response(token)
            try:
                data = _serial_exchange(link, frame, response, timeout, sending)
                report({"ok": True, "data": data})
            except DeviceError as exc:
                report({"ok": False, "error": str(exc)})
            except Exception as exc:
                report({"ok": False, "error": _session_failure_message(op, sending, exc), "fatal": True})
                return


async def _bluetooth_session(config, timeout):
    from bleak import BleakClient
    device = await _ble_resolve(config, timeout)
    queue = asyncio.Queue(maxsize=MAX_FRAME // 20 + 8)
    overflow = False
    disconnected_event = asyncio.Event()

    def receive(_, data):
        nonlocal overflow
        if queue.full():
            overflow = True
        else:
            queue.put_nowait(bytes(data))

    def disconnected(_):
        disconnected_event.set()
        try:
            queue.put_nowait(b"")
        except asyncio.QueueFull:
            pass

    async def exchange_once(client, rx, frame, response, sending):
        if disconnected_event.is_set() or not client.is_connected:
            raise OSError("BLE connection closed")
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

    async with BleakClient(device, disconnected_callback=disconnected, timeout=timeout) as client:
        service = client.services.get_service(SERVICE)
        if service is None:
            raise RuntimeError("The peer has no MicroMeerschaum action service")
        rx, tx = service.get_characteristic(RX), service.get_characteristic(TX)
        if rx is None or tx is None:
            raise RuntimeError("Incomplete MicroMeerschaum action service")
        await client.start_notify(tx, receive)
        report({"ok": True, "ready": True})
        loop = asyncio.get_running_loop()
        while True:
            raw = await loop.run_in_executor(None, sys.stdin.readline)
            if not raw:
                return
            raw = raw.strip()
            if not raw:
                continue
            try:
                request = json.loads(raw)
                op, token, frame = _build_frame(request)
            except Exception as exc:
                report({"ok": False, "error": str(exc) or type(exc).__name__})
                continue
            sending = [False]
            response = Response(token)
            try:
                data = await asyncio.wait_for(
                    exchange_once(client, rx, frame, response, sending), timeout
                )
                report({"ok": True, "data": data})
            except DeviceError as exc:
                report({"ok": False, "error": str(exc)})
            except Exception as exc:
                report({"ok": False, "error": _session_failure_message(op, sending, exc), "fatal": True})
                return


def run_session(config):
    """Hold one open serial port or bonded BLE connection across multiple requests."""
    timeout = _validate_timeout(config)
    transport = config.get("transport", "serial")
    if transport in ("usb", "serial"):
        _serial_session(config, timeout)
    elif transport == "ble":
        asyncio.run(_bluetooth_session(config, timeout))
    else:
        raise ValueError("transport must be serial or ble")


def guarded_write(config, request):
    token = secrets.token_hex(8)
    guard = WriteGuard(config, request["key"], token)
    try:
        report({"pending": guard.instruction("Outcome unknown after sending write.")})
        try:
            data = exchange(config, request, token)
            if not isinstance(data, str):
                raise UnknownOutcome("Outcome unknown after sending write: invalid acknowledgement")
        except (UnknownOutcome, DeviceError) as exc:
            # v1 device errors may include confirmed partial groups and uncertain
            # commits. Preserve the complete error for the host's subsequent attempts.
            raise RuntimeError(guard.failed(str(exc))) from exc
        except Exception:
            guard.clear()  # exchange failed before it attempted transmission
            raise
        report({"ok": True, "data": data})
        # Release only after the caller has received this definitive success.
        # Process termination or a broken stdout/stdin leaves the incident pending.
        if sys.stdin.readline() == "ack\n":
            guard.clear()
    finally:
        guard.close()


if __name__ == "__main__":
    if "WriteGuard" not in globals():  # Running the source directly.
        from write_guard import WriteGuard, pending_write, reconcile
    try:
        inputs = json.loads(sys.stdin.readline())
        if inputs.get("mode") == "session":
            run_session(inputs["config"])
        else:
            config, request = inputs["config"], inputs["request"]
            if request["op"] == "write":
                guarded_write(config, request)
            elif request["op"] == "reconcile":
                report({"ok": True, "data": reconcile(config, request["token"])})
            elif request["op"] == "pending_write":
                report({"ok": True, "data": pending_write(config, request["key"], request["label"])})
            else:
                report({"ok": True, "data": exchange(config, request)})
    except KeyboardInterrupt:
        report({"ok": False, "error": "Interrupted. Any sent action or write has an unknown outcome. Inspect the device before retrying.", "fatal": True})
    except Exception as failure:
        report({"ok": False, "error": str(failure), "fatal": True})
