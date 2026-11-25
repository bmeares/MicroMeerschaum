"""Boot the 8 MiB ESP32-S3 release image twice and exercise its public APIs."""
import argparse
import contextlib
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time


@contextlib.contextmanager
def emulator(executable, flash, elf):
    with socket.socket() as available:
        available.bind(("127.0.0.1", 0))
        port = available.getsockname()[1]
    args = [executable, "--chip", "esp32s3", "--firmware", flash, "--psram-size", "2M",
            "--uart-tcp", f"127.0.0.1:{port}", "--save-state"]
    if elf:
        args.extend(["--elf", elf])
    process = subprocess.Popen(args, stdin=subprocess.DEVNULL)
    try:
        for _ in range(150):
            if process.poll() is not None:
                raise RuntimeError(f"Emulator exited with {process.returncode}")
            try:
                serial = socket.create_connection(("127.0.0.1", port), timeout=0.5)
                break
            except ConnectionRefusedError:
                time.sleep(0.1)
        else:
            raise TimeoutError("Emulator UART did not open")
        with serial:
            yield serial
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def until(serial, delimiter):
    output = bytearray()
    end = time.monotonic() + 90
    while delimiter not in output and time.monotonic() < end:
        try:
            chunk = serial.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            break
        output.extend(chunk)
    if delimiter not in output:
        raise RuntimeError(f"Missing {delimiter!r} in {output[-1000:]!r}")
    return output


def run_board(serial, previous):
    serial.sendall(b"\r")
    until(serial, b">>> ")
    serial.sendall(b"\x01")
    until(serial, b"raw REPL; CTRL-B to exit\r\n>")
    script = f"""import _mmstorage, mm_https, mm_ble, aioble
from micromeerschaum import provision
_mmstorage.mount({not previous})
client = mm_https.open('https://example.org/micromeerschaum/v1/batch', 'fixture')
client.close()
p = provision('sensor', 'room', '/mm/room.mm', 11, 1, 22, create={not previous})
assert p.progress()['high'] == {previous}
ok, msg = p.sync({{'tick': {previous+1}}})
assert ok, msg
assert p.read_doc({previous+1})['tick'] == {previous+1}
p.close()
columns = (('tick', 'int', True), ('value', 'int', False))
s = provision('sensor', 'observed', '/mm/weather.db', 11, 1, 22, instance='sql:local',
              columns=columns if {not previous} else None,
              axis='tick' if {not previous} else None, create={not previous})
assert s.get_docs() == {([{'tick': 1, 'value': 20}] if previous else [])!r}
ok, msg = s.sync({{'tick': 1, 'value': {20+previous}}})
assert ok, msg
assert s.get_sync_time() == 1
assert s.progress()['high'] == {previous+1}
s.close()
print('Committed', {previous+1})
"""
    # A virtual UART also has a finite hardware FIFO. Pace raw REPL input.
    for start in range(0, len(script), 16):
        serial.sendall(script[start:start+16].encode())
        time.sleep(0.06)
    serial.sendall(b"\x04")
    reply = until(serial, b"\x04>")
    if f"Committed {previous+1}\r\n\x04\x04>".encode() not in reply:
        raise RuntimeError(f"Board test failed: {reply!r}")
    print(f"Boot {previous+1} committed the next journal and SQLite sequences", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--emulator", default="esp-emu")
    args = parser.parse_args()
    data = args.image.read_bytes()
    capacity = 8 << 20
    if not 0 < len(data) <= capacity:
        parser.error("expected an 8 MiB board image")
    with tempfile.TemporaryDirectory() as temp_dir:
        flash = Path(temp_dir) / "flash.bin"
        flash.write_bytes(data + bytes([255]) * (capacity - len(data)))
        for previous in (0, 1):
            with emulator(args.emulator, flash, args.elf) as serial:
                run_board(serial, previous)


if __name__ == "__main__":
    main()
