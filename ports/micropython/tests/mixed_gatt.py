# SPDX-License-Identifier: Apache-2.0
"""Usage: python mixed_gatt.py /path/to/mm_gatt_test_node /path/to/micropython"""
from contextlib import ExitStack
from pathlib import Path
from socket import socketpair
from subprocess import Popen, check_output, run
from tempfile import TemporaryDirectory
import sys


def exercise(root, implementations):
    commands = [[*impl, role, str(root / role)] for role, impl in
                zip(("origin", "relay", "dest"), implementations)]
    origin, relay, dest = commands

    def status(node):
        return check_output([*node, "status"], text=True).strip().split()

    def exchange(sender, receiver, lose):
        with ExitStack() as cleanup:
            a, b = socketpair()
            cleanup.enter_context(a)
            cleanup.enter_context(b)
            def spawn(command, sock):
                process = Popen(command, stdin=sock, stdout=sock)
                cleanup.enter_context(process)
                cleanup.callback(process.kill)
                sock.close()
                return process
            server = spawn([*receiver, "drop" if lose else "serve"], b)
            client = spawn([*sender, "send"], a)
            assert client.wait(timeout=10) == (3 if lose else 0)
            assert server.wait(timeout=10) == 0

    run([*origin, "seed"], check=True)
    original = status(origin)[2]
    for sender, receiver in ((origin, relay), (relay, dest)):
        exchange(sender, receiver, True)
        assert status(sender) == ["1", "0", original]
        assert status(receiver) == ["1", "0", original]
        exchange(sender, receiver, False)
        assert status(sender) == ["1", "1", "-"]
        assert status(receiver) == ["1", "0", original]
        Path(sender[-1]).unlink()


if __name__ == "__main__":
    native = [sys.argv[1]]
    mp = [sys.argv[2], str(Path(__file__).with_name("gatt_node.py"))]
    for nodes in ((native, mp, native), (mp, native, mp)):
        with TemporaryDirectory() as dirname:
            exercise(Path(dirname), nodes)
    print("Both runtime assignments recovered original bytes after custody and destination reply loss.")
