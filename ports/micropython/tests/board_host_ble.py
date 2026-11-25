#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the native micro plugin, Bleak and BlueZ against ESP32-S3 firmware.

Run in a disposable Linux VM with bluetoothd running and hci_vhci loaded.
The firmware and host use their own SMP stacks and exchange ATT packets.
"""

import argparse
import asyncio
import dataclasses
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys
import tempfile

from bleak import BleakScanner
from bumble import hci
from dbus_fast import BusType, Message, MessageType
from dbus_fast.aio import MessageBus
from dbus_fast.service import ServiceInterface, method

from board_ble_pair import ADDRESSES, Board, Controller, LocalLink, open_transport
from board_instance import WORKLOAD
from management_pair import SERVER


class FirmwareLink(LocalLink):
    """Route ACL by the address the controller used to establish each connection."""

    def send_acl_data(self, sender_controller, destination_address, transport, data):
        # Bumble 0.0.236 otherwise uses its last scan address. BlueZ can scan
        # with a random address and connect with its public identity address.
        connection = sender_controller.le_connections[destination_address]
        peer = self.find_le_controller(destination_address)
        asyncio.get_running_loop().call_soon(
            peer.on_link_acl_data, connection.self_address, transport, data
        )


class DesktopController(Controller):
    """Honor Linux's selected Enhanced Connection Complete event on fixed-address links."""

    def send_hci_packet(self, packet):
        mask = 1 << (hci.HCI_LE_ENHANCED_CONNECTION_COMPLETE_EVENT - 1)
        if (
            isinstance(packet, hci.HCI_LE_Connection_Complete_Event)
            and self.le_event_mask & mask
        ):
            values = {
                field.name: getattr(packet, field.name)
                for field in dataclasses.fields(packet)
            }
            packet = hci.HCI_LE_Enhanced_Connection_Complete_Event(
                **values,
                local_resolvable_private_address=hci.Address.ANY,
                peer_resolvable_private_address=hci.Address.ANY,
            )
        super().send_hci_packet(packet)


class KeyboardAgent(ServiceInterface):
    def __init__(self, code, peer_path):
        super().__init__("org.bluez.Agent1")
        self.code = code
        self.peer_path = peer_path
        self.asked = False

    @method()
    def RequestPasskey(self, device: "o") -> "u":
        if device != self.peer_path:
            raise ValueError("unexpected commissioning peer")
        self.asked = True
        return self.code

    @method()
    def Release(self):
        pass


async def bluez(bus, path, interface, member, signature="", body=None):
    reply = await asyncio.wait_for(
        bus.call(
            Message(
                destination="org.bluez",
                path=path,
                interface=interface,
                member=member,
                signature=signature,
                body=body or [],
            )
        ),
        40,
    )
    if reply.message_type == MessageType.ERROR:
        raise RuntimeError((reply.error_name, reply.body))


def plugin_environment(root, args):
    package = root / "plugins" / "micro"
    package.mkdir(parents=True)
    shutil.copy2(args.library, package / "libmicro.so")
    shutil.copy2(
        Path(__file__).resolve().parents[3] / "receiver/plugin/plugin.toml", package
    )
    config = {
        "meerschaum": {
            "instance": "sql:local",
            "connectors": {
                "micro": {
                    "bench": {
                        "transport": "ble",
                        "address": ADDRESSES[0],
                        "timeout": 15,
                        "python": sys.executable,
                    }
                },
                "sql": {
                    "local": {"flavor": "sqlite", "database": str(root / "local.db")}
                },
            },
        },
    }
    return dict(
        os.environ,
        XDG_STATE_HOME=str(root / "state"),
        MRSM_ROOT_DIR=str(root),
        MRSM_PLUGINS_DIR=str(package.parent),
        MRSM_CONFIG=json.dumps(config),
        MRSM_NO_AUTO_INSTALL="1",
        MRSM_NO_UPDATE_CHECK="1",
    )


async def qualify(args, root):
    env = plugin_environment(root, args)

    async def run(*arguments, success=True, expected=None):
        proc = await asyncio.create_subprocess_exec(
            args.host_python,
            *arguments,
            cwd=root,
            env=env,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        try:
            output, _ = await asyncio.wait_for(proc.communicate(), 180)
        finally:
            if proc.returncode is None:
                proc.kill()
                await proc.wait()
        text = output.decode()
        print(text, flush=True)
        assert (proc.returncode == 0) == success, (arguments, text)
        assert expected is None or expected in text, (expected, text)

    async def action(line, **options):
        await run(
            "-m", "meerschaum", "micro", "action", line, "-i", "micro:bench", **options
        )

    link = FirmwareLink()
    board = Board(0, link, args, root)
    async with await open_transport("vhci") as vhci:
        DesktopController(
            "desktop",
            host_source=vhci.source,
            host_sink=vhci.sink,
            public_address=ADDRESSES[1],
            link=link,
        )
        try:
            await board.start()
            example = (
                Path(__file__).resolve().parents[3]
                / "examples/micropython/offline_relay.py"
            )
            await board.execute(example.read_text())
            await board.execute(
                "import aioble, asyncio, _mmstorage\n"
                "from aioble.core import register_irq_handler\n"
                "def observe(event, data):\n"
                "    if event == 1:\n        print('CENTRAL',data[1],bytes(data[2]))\n"
                "    if event == 31:\n        print('PASSKEY ACTION',data[1])\n"
                "    if event == 28:\n        print('ENCRYPTION', tuple(data[1:]))\n"
                "register_irq_handler(observe, None)\n"
                "_mmstorage.mount(True)\nidentity_address()\n"
                f"other = aioble.Device(0, {ADDRESSES[1]!r})\n"
            )
            await board.execute(SERVER)
            code = secrets.randbelow(1000000)
            await board.paste(
                f"print('LISTENING')\nasyncio.run(enroll_receiver(other, '{code:06d}'))\nprint('ENROLLED')"
            )
            await board.until(b"LISTENING\r\n")
            await action(
                "device echo pre-enrollment", success=False, expected="Enroll the peer"
            )
            device = await BleakScanner.find_device_by_address(ADDRESSES[0], timeout=10)
            assert device, "commissioning advertisement missing"
            peer_path = device.details["path"]
            bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
            try:
                agent = KeyboardAgent(code, peer_path)
                bus.export("/testagent", agent)
                await bluez(
                    bus,
                    "/org/bluez",
                    "org.bluez.AgentManager1",
                    "RegisterAgent",
                    "os",
                    ["/testagent", "KeyboardOnly"],
                )
                await bluez(
                    bus,
                    "/org/bluez",
                    "org.bluez.AgentManager1",
                    "RequestDefaultAgent",
                    "o",
                    ["/testagent"],
                )
                await bluez(bus, peer_path, "org.bluez.Device1", "Pair")
                report = await board.result()
                assert (
                    agent.asked
                    and b"ENCRYPTION (1, 1, 1, 16)" in report
                    and b"ENROLLED" in report
                )
            finally:
                bus.disconnect()
            await board.paste(
                "async def serve():\n    while True:\n        await peripheral.serve_once()\n"
                "print('LISTENING')\nasyncio.run(serve())"
            )
            await board.until(b"LISTENING\r\n")
            await action("device echo desktop-echo", expected="desktop-echo")
            await action("device private", success=False, expected="action unavailable")
            await run(
                "-m",
                "meerschaum",
                "show",
                "pipes",
                "-i",
                "micro:bench",
                expected="room",
            )
            await run("-c", WORKLOAD, "1", "room")
            print(
                "PASS: desktop CLI, action permissions and SQLite to firmware to SQLite over BLE"
            )
        finally:
            if sys.exc_info()[0] is not None and hasattr(board, "read"):
                try:
                    print(
                        "Firmware console:",
                        await asyncio.wait_for(board.read.read(4096), 2),
                        flush=True,
                    )
                except TimeoutError:
                    pass
            await board.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--emulator", default="esp-emu")
    parser.add_argument("--efuse-tool", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument(
        "--host-python", default=sys.executable, help="interpreter with Meerschaum"
    )
    args = parser.parse_args()
    if subprocess.run(["systemd-detect-virt", "--vm", "--quiet"]).returncode:
        parser.error("run the test inside a disposable Linux virtual machine")
    if any(Path("/sys/class/bluetooth").iterdir()):
        parser.error("use a guest with no existing Bluetooth adapters")
    with tempfile.TemporaryDirectory() as path:
        asyncio.run(qualify(args, Path(path)))


if __name__ == "__main__":
    main()
