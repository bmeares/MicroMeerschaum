#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the MicroMeerschaum custody example on two ESP32-S3 images with a virtual BLE link.

Requires espressif/esp-emulator v1.2.0 and bumble==0.0.236. The ESP32-S3
binary, matching ELF and example are provided as arguments. All radio
traffic stays on the local loopback link. Test flash files are private copies.
"""

import argparse
import asyncio
import contextlib
import lzma
import re
import secrets
import signal
import socket
import sys
import tempfile
from pathlib import Path

from bumble import hci
from bumble.controller import Controller
from bumble.link import LocalLink
from bumble.transport import open_transport

ADDRESSES = ("24:0a:c4:01:00:02", "24:0a:c4:02:00:02")


# Espressif's esp-emulator v1.2.0 tools/bumble_test.py supplies this workaround
# for resolving-list housekeeping missing from Bumble's controller. SMP, ACL
# traffic, encryption and authentication run through their normal handlers.
def ack_ok(self, command):
    params = hci.HCI_GenericReturnParameters(data=b"\x00")
    if isinstance(command, hci.HCI_SyncCommand):
        return params
    self.send_hci_packet(
        hci.HCI_Command_Complete_Event(
            num_hci_command_packets=1,
            command_opcode=command.op_code,
            return_parameters=params,
        )
    )


for name in (
    "set_privacy_mode",
    "clear_resolving_list",
    "add_device_to_resolving_list",
    "remove_device_from_resolving_list",
):
    nm = f"on_hci_le_{name}_command"
    if not hasattr(Controller, nm):
        setattr(Controller, nm, ack_ok)


def unused():
    with socket.socket() as available:
        available.bind(("127.0.0.1", 0))
        return available.getsockname()[1]


class Board:
    def __init__(self, n, link, args, workdir):
        self.args = args
        self.link = link
        self.address = ADDRESSES[n]
        self.name = ["sink", "source"][n]
        self.hci = unused()
        self.uart = unused()
        self.efuse = workdir / (self.name + ".efuse")
        self.flash = workdir / (self.name + ".bin")
        self.files = contextlib.ExitStack()
        self.log = self.files.enter_context((workdir / (self.name + ".emu.log")).open("wb"))
        self.transcript = self.files.enter_context((workdir / (self.name + ".uart.log")).open("wb"))

    async def boot(self):
        self.transport = await open_transport(f"tcp-server:127.0.0.1:{self.hci}")
        self.controller = Controller(
            self.name,
            host_source=self.transport.source,
            host_sink=self.transport.sink,
            link=self.link,
            public_address=self.address,
        )
        self.controller.random_address = hci.Address(
            self.address, hci.Address.PUBLIC_DEVICE_ADDRESS
        )
        data = self.args.image.read_bytes()
        if not 0 < len(data) <= 8 << 20:
            raise ValueError("expected an 8 MiB ESP32-S3 flash image")
        self.flash.write_bytes(data + b"\xff" * ((8 << 20) - len(data)))
        efuse = self.efuse
        args = [
            sys.executable,
            self.args.efuse_tool,
            "--chip",
            "esp32s3",
            "--chip-rev",
            "0.0",
            "--mac",
            self.address[:-2] + "00",
            "-o",
            efuse,
        ]
        p = await asyncio.create_subprocess_exec(*args, stdout=self.log, stderr=self.log)
        if await p.wait():
            raise RuntimeError("eFuse generation failed")
        self.p = await asyncio.create_subprocess_exec(
            self.args.emulator,
            "--chip",
            "esp32s3",
            "--firmware",
            self.flash,
            "--elf",
            self.args.elf,
            "--psram-size",
            "2M",
            "--efuse",
            efuse,
            "--net",
            "user,net=10.0.2.0/24",
            "--uart-tcp",
            f"127.0.0.1:{self.uart}",
            "--ble-hci",
            f"tcp:127.0.0.1:{self.hci}",
            stdin=asyncio.subprocess.DEVNULL,
            stdout=self.log,
            stderr=self.log,
        )
        for _ in range(150):
            if self.p.returncode is not None:
                raise RuntimeError(f"{self.name} emulator exited with {self.p.returncode}")
            try:
                self.read, self.write = await asyncio.open_connection("127.0.0.1", self.uart)
                break
            except OSError:
                await asyncio.sleep(0.11)
        else:
            raise RuntimeError("no UART")

    async def start(self):
        await self.boot()
        self.write.write(b"\r")
        await self.until(b">>> ")
        self.write.write(b"\x01")
        await self.until(b"raw REPL; CTRL-B to exit\r\n>")

    async def until(self, value, timeout=90):
        try:
            out = await asyncio.wait_for(self.read.readuntil(value), timeout)
        except asyncio.IncompleteReadError as error:
            self.transcript.write(error.partial)
            raise RuntimeError(f"{self.name} UART closed while waiting for {value!r}") from error
        self.transcript.write(out)
        self.transcript.flush()
        return out

    async def paste(self, code):
        # The emulated UART has the board's finite receive FIFO; pace raw REPL input.
        data = code.encode()
        for i in range(0, len(data), 16):
            self.write.write(data[i : i + 16])
            await self.write.drain()
            await asyncio.sleep(0.06)
        self.write.write(b"\x04")

    async def execute(self, code, timeout=90):
        await self.paste(code)
        return await self.result(timeout)

    async def result(self, timeout=90):
        data = await self.until(b"\x04>", timeout)
        print(self.name, data.decode(errors="replace"), flush=True)
        if b"Traceback" in data:
            raise RuntimeError(self.name)
        return data

    async def close(self):
        if hasattr(self, "p") and self.p.returncode is None:
            self.p.send_signal(signal.SIGINT)
            try:
                await asyncio.wait_for(self.p.wait(), 10)
            except TimeoutError:
                self.p.kill()
                await self.p.wait()
        if hasattr(self, "transport"):
            self.transport.server.close()
            await self.transport.server.wait_closed()
        if hasattr(self, "write"):
            self.write.close()
            await self.write.wait_closed()
        self.files.close()


class ReceiptLossLink(LocalLink):
    """Lose precisely one complete ATT Read Response containing a receipt fragment."""

    def __init__(self):
        super().__init__()
        self.lost = False
        self.fragment_count = 0

    def send_acl_data(self, sender_controller, destination_address, transport, data):
        if (
            sender_controller.name == "sink"
            and data[2:4] == b"\x04\x00"
            and data[4:6] == b"\x0b\x03"
        ):
            self.fragment_count += 1
            if not self.lost:
                self.lost = True
                print(
                    "Bumble withheld the first ATT Read Response carrying a receipt",
                    flush=True,
                )
                return
        return super().send_acl_data(sender_controller, destination_address, transport, data)


async def exchange_window(sink, src, servercode, clientcode):
    # Wait for the receiver before asking the sender to connect.
    await sink.paste(servercode)
    await sink.until(b"LISTENING\r\n", 20)
    await src.paste(clientcode)
    return await asyncio.gather(sink.result(120), src.result(120))


async def test_pair(args, workdir):
    if args.elf.suffix == ".xz":
        target = workdir / "micropython.elf"
        target.write_bytes(lzma.decompress(args.elf.read_bytes()))
        args.elf = target
    link = ReceiptLossLink()
    sink, src = boards = [Board(n, link, args, workdir) for n in (0, 1)]
    try:
        await asyncio.gather(*(b.start() for b in boards))
        print("Both firmwares booted", flush=True)
        module = args.example.read_text()
        await asyncio.gather(*(b.execute(module) for b in boards))
        setup = """import aioble, asyncio, _mmstorage, binascii
from aioble.core import register_irq_handler
def observe(event,data):
    if event == 28:
        print('ENCRYPTION',tuple(data[1:]))
    if event == 31:
        print('PASSKEY ACTION',data[1])
register_irq_handler(observe,None)
_mmstorage.mount(True)
print('IDENTITY', identity_address())
"""
        await asyncio.gather(*(b.execute(setup) for b in boards))
        await asyncio.gather(
            src.execute(f"other=aioble.Device(0,{sink.address!r})"),
            sink.execute(f"other=aioble.Device(0,{src.address!r})"),
        )
        code = f"{secrets.randbelow(1000000):06d}"
        issued = await exchange_window(
            sink,
            src,
            f"print('LISTENING')\nasyncio.run(enroll_receiver(other,'{code}'))\nprint('ENROLLED')",
            f"asyncio.run(enroll_sender(other,'{code}'))\nprint('ENROLLED')",
        )
        for report, action in zip(issued, (3, 2)):
            assert f"PASSKEY ACTION {action}".encode() in report
            assert b"ENCRYPTION (1, 1, 1, 16)" in report
            assert b"ENROLLED" in report
        print("Authenticated enrollment finished on both devices", flush=True)
        doc = {"temperature": 19, "tick": 21, "probe": "A1"}
        seed, _ = await asyncio.gather(
            src.execute(
                f"pipe=open_role('origin','/mm/origin.mm',True)\nassert pipe.sync({doc!r})[0]\n"
                "print('ORIGINAL',binascii.hexlify(pipe.read_record(1)))"
            ),
            sink.execute("pipe=open_role('relay','/mm/relay.mm',True)"),
        )
        frame = re.search(rb"ORIGINAL b'([0-9a-f]+)'", seed).group(1)
        sinkcode = """print('LISTENING')
asyncio.run(receive_window(pipe,other,1))
state = pipe.progress()
print('RELAY STATE',state)
assert state['prefix'] == state['high'] == 1 and state['missed'] == 0
assert pipe.read_doc(1) == DOC
print('RELAY BYTES',binascii.hexlify(pipe.read_record(1)))
""".replace("DOC", repr(doc))
        response, _ = await exchange_window(
            sink,
            src,
            sinkcode,
            """try:
    asyncio.run(send_window(pipe,other,2))
    raise AssertionError('Expected first receipt to time out')
except (OSError,asyncio.TimeoutError):
    print('LOST REPLY', pipe.progress())
assert pipe.progress()['confirmed'] == 0
""",
        )
        assert frame in response, "First receipt followed different stored record bytes"
        assert link.lost, "No ATT receipt read response was dropped"
        response, _ = await exchange_window(
            sink,
            src,
            sinkcode,
            """assert asyncio.run(send_window(pipe,other,2)) == 1
assert pipe.progress()['confirmed'] == 1
print('SOURCE CONFIRMED',pipe.progress())
""",
        )
        assert frame in response, "Retransmission changed stored record bytes"
        assert link.fragment_count >= 2
        print(
            "PASS: authenticated provisioned Pipe on two firmware instances, lost receipt, duplicate replay, durable checkpoint",
            flush=True,
        )
    finally:
        await asyncio.gather(*(b.close() for b in boards))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument(
        "--elf",
        required=True,
        type=Path,
        help="matching application ELF, optionally compressed with xz",
    )
    repo = Path(__file__).resolve().parents[3]
    parser.add_argument(
        "--example", type=Path, default=repo / "examples/micropython/offline_relay.py"
    )
    parser.add_argument("--emulator", default="esp-emu")
    parser.add_argument(
        "--efuse-tool",
        required=True,
        type=Path,
        help="path to tools/make-efuse.py from esp-emulator v1.2.0",
    )
    parser.add_argument("--artifacts", type=Path, help="retain UART and emulator logs")
    args = parser.parse_args()
    if args.artifacts:
        workdir = args.artifacts
        workdir.mkdir(parents=True, exist_ok=True)
        if any(workdir.iterdir()):
            parser.error("--artifacts must name an empty directory")
        asyncio.run(test_pair(args, workdir))
    else:
        with tempfile.TemporaryDirectory() as workdir:
            asyncio.run(test_pair(args, Path(workdir)))


if __name__ == "__main__":
    main()
