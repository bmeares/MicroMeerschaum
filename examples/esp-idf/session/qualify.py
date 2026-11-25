#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the native ESP32-S3 management example on a virtual wired and BLE link."""
import argparse
import asyncio
import json
import sys
import tempfile
from pathlib import Path

from bumble import hci
from bumble.att import ATT_Error
from bumble.core import UUID
from bumble.device import Device, Peer
from bumble.host import Host
from bumble.pairing import PairingConfig, PairingDelegate

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "ports/micropython/tests"))
from board_ble_pair import ADDRESSES, Board, Controller, LocalLink

RX = UUID("2f725f70-9771-4f95-bb68-5e47ea680002")
TX = UUID("2f725f70-9771-4f95-bb68-5e47ea680003")


def frame(ident, op, **fields):
    return b"@micro " + json.dumps({"v": 1, "id": ident, "op": op, **fields}).encode() + b"\n"


class Keyboard(PairingDelegate):
    def __init__(self, board):
        super().__init__(io_capability=PairingDelegate.KEYBOARD_INPUT_ONLY)
        self.board = board

    async def get_number(self):
        await self.board.until(b"Pairing passkey ")
        digits = (await self.board.until(b"\r\n")).strip()
        assert len(digits) == 6 and digits.isdigit()
        return int(digits)


async def run(args, directory):
    link = LocalLink()
    board = Board(0, link, args, directory)
    try:
        await board.boot()
        await board.until(b"mm> ")
        # Send UART bytes in FIFO-sized fragments, as a USB bridge does.
        async def wired(data):
            for i in range(0, len(data), 16):
                board.write.write(data[i:i + 16])
                await board.write.drain()
                await asyncio.sleep(0.02)

        await wired(b"show uptime\r")
        assert b" ms" in await board.until(b"mm> ")
        await wired(frame("usb", "catalog"))
        await board.until(b"@micro ")
        result = json.loads(await board.until(b"\n"))
        assert result["id"] == "usb" and result["data"]["items"][0]["read"]
        print("Native USB console and management catalog passed", flush=True)

        ctrl = Controller("central", link=link, public_address=ADDRESSES[1])
        host = Host()
        host.controller = ctrl
        ctrl.host = host
        central = Device(name="Management test", host=host)
        await central.power_on()
        # Match LocalLink's ACL source to this controller's public identity.
        ctrl.random_address = hci.Address(ADDRESSES[1], hci.Address.PUBLIC_DEVICE_ADDRESS)
        central.pairing_config_factory = lambda _conn: PairingConfig(
            sc=True, mitm=True, bonding=True, delegate=Keyboard(board),
            identity_address_type=PairingConfig.AddressType.PUBLIC,
        )
        adv = asyncio.Event()
        central.on("advertisement", lambda event: adv.set())
        await central.start_scanning()
        await asyncio.wait_for(adv.wait(), 40)
        await central.stop_scanning()
        conn = await central.connect(hci.Address(ADDRESSES[0], hci.Address.PUBLIC_DEVICE_ADDRESS),
                                     own_address_type=hci.OwnAddressType.PUBLIC, timeout=30)
        async def characteristics(conn):
            peer = Peer(conn)
            await peer.discover_services()
            await peer.discover_characteristics()
            return peer.get_characteristics_by_uuid(RX)[0], peer.get_characteristics_by_uuid(TX)[0]

        try:
            rx, tx = await characteristics(conn)
            try:
                await rx.write_value(b"@micro ", with_response=True)
            except ATT_Error:
                pass
            else:
                raise AssertionError("unpaired write passed")
            await tx.discover_descriptors()
            cccd = tx.get_descriptor(UUID.from_16_bits(0x2902))
            try:
                await cccd.write_value(b"\x01\x00", with_response=True)
            except ATT_Error:
                pass
            else:
                raise AssertionError("unpaired notification subscription passed")
            await asyncio.wait_for(conn.pair(), 90)
            await tx.discover_descriptors()
            packets = asyncio.Queue()
            await tx.subscribe(packets.put_nowait)

            async def request(ident, op, **fields):
                raw = frame(ident, op, **fields)
                for i in range(0, len(raw), 20):
                    await rx.write_value(raw[i:i + 20], with_response=True)
                result = bytearray()
                while not result.endswith(b"\n"):
                    result.extend(await asyncio.wait_for(packets.get(), 15))
                    assert len(result) <= 4096
                assert result.startswith(b"@micro ")
                obj = json.loads(result[7:])
                assert obj["v"] == 1 and obj["id"] == ident
                return obj

            assert (await request("ble", "catalog"))["data"]["items"] == result["data"]["items"]
            assert (await request("a", "action", line="show uptime"))["data"].endswith(" ms")
            key = ["sensor", "uptime", None]
            assert (await request("r", "read", key=key, limit=1))["data"][0]["microseconds"] > 0
            assert not (await request("w", "write", key=key, rows=[{"microseconds": 1}]))["ok"]
            # Disconnect after a valid request prefix, then send its suffix on
            # a restored bond. No request may span two connection generations.
            partial = frame("old", "action", line="show uptime")
            await rx.write_value(partial[:17], with_response=True)
            await conn.disconnect()
            conn = await central.connect(hci.Address(ADDRESSES[0], hci.Address.PUBLIC_DEVICE_ADDRESS),
                                         own_address_type=hci.OwnAddressType.PUBLIC, timeout=30)
            await conn.encrypt()
            rx, tx = await characteristics(conn)
            await tx.discover_descriptors()
            packets = asyncio.Queue()
            await tx.subscribe(packets.put_nowait)
            for i in range(17, len(partial), 20):
                await rx.write_value(partial[i:i + 20], with_response=True)
            assert (await request("fresh", "complete", line="show u"))["data"] == ["show uptime"]
            print("Native bonded management GATT passed fragmentation, read, denial and reconnect", flush=True)
        finally:
            await conn.disconnect()
    finally:
        await board.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("image", "--elf", "--emulator", "--efuse-tool"):
        parser.add_argument(name, type=Path, **({"required": True} if name.startswith("-") else {}))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        asyncio.run(run(args, Path(directory)))
