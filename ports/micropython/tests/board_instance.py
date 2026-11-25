"""Exercise a real Meerschaum instance connector against an ESP32-S3 on persistent flash."""

import argparse
import contextlib
import json
import os
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import tty
from pathlib import Path

from board import emulator, until

APP = """from micromeerschaum import provision, expose, make_action
from mm_session import DeviceSession, serve_usb
import _mmstorage
_mmstorage.mount(False)
p = provision('sensor', 'readings', '/mm/bench.mm', 11, 1, 22, create=False)
expose(p, read=True, write=True)
@make_action('bench status')
def status(action=None, **kw):
    return True, 'stored %d' % p.progress()['high']
s = DeviceSession(operations=('catalog','read','write','actions','action','complete'),
                  allowed_actions=('bench status','show pipes','help'))
print('INSTANCE READY')
serve_usb(s)
"""


def provision_board(uart):
    uart.sendall(b"\r")
    until(uart, b">>> ")
    uart.sendall(b"\x01")
    until(uart, b"raw REPL; CTRL-B to exit\r\n>")
    code = (
        "import _mmstorage\nfrom micromeerschaum import provision\n"
        "_mmstorage.mount(True)\n"
        "provision('sensor','readings','/mm/bench.mm',11,1,22,create=True).close()\n"
        f"with open('main.py','w') as f:\n    f.write({APP!r})\n"
        "import os\nos.sync()\nprint('PROVISIONED')\n"
    ).encode()
    for i in range(0, len(code), 16):
        uart.sendall(code[i : i + 16])
        time.sleep(0.05)
    uart.sendall(b"\x04")
    result = until(uart, b"\x04>")
    if b"PROVISIONED\r\n\x04\x04>" not in result:
        raise RuntimeError(result)


@contextlib.contextmanager
def serial_port(uart):
    """The ESP emulator exposes UART bytes on a socket. Give pyserial a real TTY."""
    master, slave = pty.openpty()
    tty.setraw(slave)
    end = threading.Event()

    def pump():
        while not end.is_set():
            readable, _, _ = select.select([uart, master], [], [], 0.1)
            if uart in readable:
                reply = uart.recv(4096)
                if not reply:
                    return
                os.write(master, reply)
            if master in readable:
                # The emulator's UART FIFO, like a literal serial port, has limited depth.
                data = os.read(master, 4096)
                for i in range(0, len(data), 16):
                    uart.sendall(data[i : i + 16])
                    time.sleep(0.003)

    worker = threading.Thread(target=pump)
    worker.start()
    try:
        yield os.ttyname(slave)
    finally:
        end.set()
        worker.join()
        os.close(master)
        os.close(slave)


WORKLOAD = """
import sys, meerschaum as mrsm
boot = int(sys.argv[1])
device = mrsm.Pipe('sensor', sys.argv[2], instance='micro:bench')
local = mrsm.Pipe('host', 'readings', instance='sql:local', columns={'primary':'tick'})
rows = [{'tick':1,'value':17},{'tick':2,'value':21}]
assert device.exists(), device
if boot == 1:
    ok, msg = local.sync(rows)
    assert ok, msg
    ok, msg = device.sync(sorted(local.get_docs(limit=2),key=lambda r:r['tick']))
    assert ok, msg
assert device.get_docs(limit=1) == rows[:1]
assert device.get_docs(limit=2) == rows
mirror = mrsm.Pipe('from_device', 'readings', instance='sql:local', columns={'primary':'tick'})
ok, msg = mirror.sync(device.get_docs(limit=2))
assert ok, msg
assert sorted(mirror.get_docs(limit=2),key=lambda r:r['tick']) == rows
print('Boot %d: ordinary Meerschaum Pipes verified two rows on device and in SQLite' % boot)
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--emulator", default="esp-emu")
    parser.add_argument(
        "--library", type=Path, required=True, help="built micro plugin"
    )
    parser.add_argument(
        "--transport-python", default=sys.executable, help="Python with pyserial"
    )
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        flash = root / "flash.bin"
        image = args.image.read_bytes()
        flash.write_bytes(image + b"\xff" * (8 * 1024 * 1024 - len(image)))
        (root / "mrsm").mkdir()
        plugins = root / "plugins"
        package = plugins / "micro"
        package.mkdir(parents=True)
        shutil.copy2(args.library, package / "libmicro.so")
        descriptor = Path(__file__).resolve().parents[3] / "receiver/plugin/plugin.toml"
        shutil.copy2(descriptor, package / "plugin.toml")
        with emulator(args.emulator, flash, args.elf) as uart:
            provision_board(uart)
        for boot in (1, 2):
            with emulator(args.emulator, flash, args.elf) as uart:
                until(uart, b"INSTANCE READY\r\n")
                with serial_port(uart) as port:
                    connectors = {
                        "micro": {
                            "bench": {
                                "transport": "usb",
                                "port": port,
                                "python": str(args.transport_python),
                                "timeout": 10,
                            }
                        },
                        "sql": {
                            "local": {
                                "flavor": "sqlite",
                                "database": str(root / "local.db"),
                            }
                        },
                    }
                    env = dict(
                        os.environ,
                        MRSM_PLUGINS_DIR=str(plugins),
                        XDG_STATE_HOME=str(root / "state"),
                        MRSM_ROOT_DIR=str(root / "mrsm"),
                        MRSM_CONFIG=json.dumps(
                            {
                                "meerschaum": {
                                    "connectors": connectors,
                                    "instance": "sql:local",
                                }
                            }
                        ),
                    )
                    output = subprocess.check_output(
                        ["mrsm", "show", "pipes", "-i", "micro:bench"],
                        env=env,
                        cwd=root,
                        text=True,
                        timeout=60,
                    )
                    assert "readings" in output, output
                    subprocess.run(
                        [sys.executable, "-c", WORKLOAD, str(boot), "readings"],
                        env=env,
                        cwd=root,
                        check=True,
                        timeout=120,
                    )


if __name__ == "__main__":
    main()
