"""Exercise an installed native receiver over TLS. Run in a fresh Meerschaum 5.1.5 environment."""

import argparse
import concurrent.futures
import contextlib
import json
import os
import secrets
import shutil
import socket
import sqlite3
import ssl
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

PLUGIN = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PLUGIN.parents[1] / "vectors"))
import generate
from generate_v1 import (
    DESTINATION,
    GENERATION,
    KEY,
    ORIGIN,
    batch,
    loss,
    receipt,
    record,
)


def checked_command(*args, **kwargs):
    result = subprocess.run(
        args, check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs
    )
    assert result.returncode == 0, result.stdout.decode(errors="replace")[-4000:]
    return result.stdout


@contextlib.contextmanager
def postgres():
    password = secrets.token_hex(16)
    cid = (
        checked_command(
            "docker",
            "run",
            "--rm",
            "-d",
            "-p",
            "127.0.0.1::5432",
            "-e",
            f"POSTGRES_PASSWORD={password}",
            "postgres:18",
        )
        .decode()
        .strip()
    )
    try:
        port = (
            checked_command("docker", "port", cid, "5432/tcp")
            .decode()
            .strip()
            .split(":")[-1]
        )
        for _ in range(100):
            probe = subprocess.run(
                ["docker", "exec", cid, "pg_isready", "-h", "127.0.0.1"],
                capture_output=True,
                check=False,
            )
            if probe.returncode == 0:
                break
            time.sleep(0.2)
        yield f"postgresql://postgres:{password}@127.0.0.1:{port}/postgres"
    finally:
        checked_command("docker", "stop", cid)


@contextlib.contextmanager
def serve(root, env):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    with (root / "api.log").open("ab") as log:
        proc = subprocess.Popen(
            [
                "mrsm",
                "start",
                "api",
                "--host",
                "127.0.0.1",
                "--port",
                str(port),
                "-i",
                "sql:mmtest",
                "--no-dash",
                "--no-webterm",
                "--keyfile",
                str(root / "tls.key"),
                "--certfile",
                str(root / "tls.pem"),
            ],
            env=env,
            cwd=root,
            stdout=log,
            stderr=log,
        )
    context = ssl.create_default_context(cafile=str(root / "tls.pem"))

    def request(path, body=None, token=None):
        headers = {"Content-Type": "application/octet-stream"}
        if token:
            headers["Authorization"] = "Bearer " + token
        req = urllib.request.Request(
            f"https://127.0.0.1:{port}" + path, data=body, headers=headers
        )
        try:
            with urllib.request.urlopen(req, context=context, timeout=30) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    try:
        for _ in range(150):
            assert proc.poll() is None, (root / "api.log").read_text()[-3000:]
            try:
                if request("/healthcheck")[0] == 200:
                    break
            except OSError:
                pass
            time.sleep(0.2)
        else:
            raise RuntimeError((root / "api.log").read_text()[-3000:])
        yield request
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def doc(value):
    # Two signed integers in the document fixture profile, with an unchanged logical row key.
    return (
        b"MMDC\x01\x02\x04tick\x03"
        + struct.pack("<q", 1)
        + b"\x05value\x03"
        + struct.pack("<q", value)
    )


def qualify_chip_sensor(root, mp, config, raw_db, token):
    repo = PLUGIN.parents[1]
    examples = repo / "examples" / "micropython"
    # Supply the S3's integer die-temperature reading and substitute a Unix volume path.
    (root / "esp32.py").write_text("def mcu_temperature(): return 37\n")
    (root / "_mmstorage.py").write_text("def mount(first_boot): pass\n")
    (root / "device_identity.py").write_text(
        f"ORIGIN = 11\nGENERATION = {secrets.randbits(63) + 1}\nDESTINATION = 22\n"
    )
    paths = [root, examples, repo / "ports" / "micropython", ".frozen"]
    mp_env = dict(os.environ, MICROPYPATH=os.pathsep.join(map(str, paths)))
    # Use the same sensor, source registration and persisted stream on both opens.
    def on_device(script):
        setup = """
import json, readings
from micromeerschaum import provision
def unix_provision(connector, metric, path, *args, **kw):
    return provision(connector, metric, "TEMP.MM", *args, **kw)
readings.provision = unix_provision
"""
        return checked_command(str(mp), "-c", setup + script, cwd=root, env=mp_env)

    reading = json.loads(on_device("""
pipe = readings.open_readings(True)
ok, message = pipe.sync()
assert ok, message
print(json.dumps(pipe.read_doc(1)))
with open("first.batch", "wb") as batch:
    batch.write(pipe.batch())
pipe.close()
"""))
    assert reading["celsius"] == 37
    overlay = json.loads(checked_command(
        sys.executable, str(examples / "receiver_config.py"), str(token.id),
        raw_db, "--instance", "sql:mmtest", cwd=root,
    ))
    env = dict(os.environ, MRSM_CONFIG=json.dumps({**config, **overlay}))
    with serve(root, env) as post:
        code, reply = post(
            "/micromeerschaum/v1/batch", (root / "first.batch").read_bytes(), token.get_api_key()
        )
        assert code == 200, (code, reply)
    (root / "first.receipt").write_bytes(reply)
    on_device("""
from device_identity import DESTINATION
pipe = readings.open_readings()
with open("first.receipt", "rb") as receipt:
    pipe.confirm(DESTINATION, receipt.read())
assert pipe.progress()["confirmed"] == 1
pipe.close()
""")
    checked_command("mrsm", "mm", "receiver", "replay", cwd=root, env=env)
    import meerschaum as mrsm

    rows = mrsm.Pipe("weather", "chip_temperature", instance="sql:mmtest").get_docs(limit=1)
    assert rows == [reading], rows
    print("First chip_sensor observation matched the hosted Pipe after TLS upload and replay.")


def qualify(root, library, db, micropython=None):
    for k in list(os.environ):
        if k.startswith("MRSM_"):
            del os.environ[k]
    plugins = root / "plugins"
    package = plugins / "micro"
    package.mkdir(parents=True)
    shutil.copy2(library, package / library.name)
    shutil.copy2(PLUGIN / "plugin.toml", package)
    output_db = db or f"sqlite:///{root / 'pipes.db'}"
    raw_db = db or f"sqlite://{root / 'raw.db'}?mode=rwc"
    os.environ.update(
        MRSM_ROOT_DIR=str(root),
        MRSM_PLUGINS_DIR=str(plugins),
        MRSM_NO_AUTO_INSTALL="1",
        MRSM_NO_UPDATE_CHECK="1",
        MRSM_SQL_MMTEST=output_db,
        MRSM_SQL_MAIN=output_db,
    )
    config = {
        "meerschaum": {
            "instance": "sql:mmtest",
            "api_instance": "sql:mmtest",
            "executor": "local",
        }
    }
    os.environ["MRSM_CONFIG"] = json.dumps(config)
    import meerschaum as mrsm
    from meerschaum.core import Token, User

    assert mrsm.__version__ in ("5.1.5", "5.1.6")
    conn = mrsm.get_connector("sql:mmtest")
    user = User(
        "device",
        secrets.token_urlsafe(24),
        instance=conn,
        type="user",
        attributes={"scopes": ["mm:ingest", "pipes:read"]},
    )
    ok, msg = conn.register_user(user)
    assert ok, msg
    tokens = [
        Token(f"sender-{i}", user=user, instance=conn, scopes=scopes)
        for i, scopes in enumerate(
            [["mm:ingest"], ["mm:ingest"], ["mm:ingest"], ["pipes:read"]]
        )
    ]
    for token in tokens:
        ok, msg = token.register()
        assert ok, msg
    direct, relay, unauthorized, unscoped = [t.get_api_key() for t in tokens]
    settings = {
        "raw_store": raw_db,
        "destination": DESTINATION,
        "grants": [
            {
                "token_id": str(t.id),
                "origin": ORIGIN if i != 2 else 1,
                "key": KEY.decode(),
                "legacy": i == 0,
            }
            for i, t in enumerate(tokens)
        ],
        "outputs": [
            {
                "origin": ORIGIN,
                "generation": str(GENERATION),
                "key": KEY.decode(),
                "connector": "weather",
                "metric": "temperature",
                "instance": "sql:mmtest",
                "columns": {"datetime": "tick"},
                "dtypes": {"tick": "int", "value": "int"},
            }
        ],
    }
    settings["outputs"].append(dict(settings["outputs"][0], metric="second_output"))
    config["plugins"] = {"mm_receiver": settings}
    env = {**os.environ, "MRSM_CONFIG": json.dumps(config)}
    checked_command("mrsm", "mm", "receiver", "init", env=env, cwd=root)
    checked_command(
        "openssl",
        "req",
        "-x509",
        "-newkey",
        "ec",
        "-pkeyopt",
        "ec_paramgen_curve:P-256",
        "-nodes",
        "-days",
        "1",
        "-subj",
        "/CN=localhost",
        "-addext",
        "subjectAltName=IP:127.0.0.1",
        "-keyout",
        str(root / "tls.key"),
        "-out",
        str(root / "tls.pem"),
    )

    v1, v0 = "/micromeerschaum/v1/batch", "/micromeerschaum/v0/batch"
    original, correction = record(1, doc(11)), record(2, doc(44))
    expected = lambda n: receipt(1, DESTINATION, through=n)
    with serve(root, env) as post:
        assert post(v1, batch([original]))[0] == 401
        assert post(v1, batch([original]), unscoped)[0] == 403
        assert post(v1, batch([original]), unauthorized)[0] == 403
        reply = post(v1, batch([correction]), direct)
        assert reply == (200, expected(0)), reply
        assert post(v1, batch([original])[:-1], direct)[0] == 400
        assert post(v1, batch([original, record(2, b"conflict")]), direct)[0] == 409
        assert post(v1, batch(), direct) == (200, expected(0))
        # A committed request whose successful response the client discards.
        post(v1, batch([original]), direct)
    with serve(root, env) as post:
        routes = [(direct, []), (relay, [(7, 45, 1)])] * 4

        def retry(route):
            token, hops = route
            return post(v1, batch([original, correction], hops), token)

        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            assert list(pool.map(retry, routes)) == [(200, expected(2))] * len(routes)
        legacy = generate.batch_header([], 1, key=KEY, origin=ORIGIN) + record(
            1, b"archive"
        )
        assert post(v1, legacy, direct)[0] == post(v0, batch(), direct)[0] == 400
        assert post(v0, legacy, relay)[0] == 403
        code, reply = post(v0, legacy, direct)
        assert code == 200 and json.loads(reply) == {
            "origin": ORIGIN,
            "pipe": KEY.decode(),
            "durable_through": 1,
        }
        replay = ["mrsm", "mm", "receiver", "replay"]
        # Inspect exact original bytes and route observations, then induce a storage failure
        # after the independent ordinary Pipe commits its first row.
        raw = conn if db else sqlite3.connect(root / "raw.db", isolation_level=None)

        def execute(sql):
            if db:
                assert raw.exec(sql) is not None
            else:
                raw.execute(sql)

        def scalar(sql):
            return raw.value(sql) if db else raw.execute(sql).fetchone()[0]

        hex_record = "encode(original,'hex')" if db else "hex(original)"
        for original_bytes in (original, correction):
            assert (
                scalar(
                    f"SELECT count(*) FROM mm_record WHERE lower({hex_record})='{original_bytes.hex()}'"
                )
                == 1
            )
        assert (
            scalar("SELECT count(*) FROM mm_record") == 3
        )  # Includes the separate legacy record.
        assert scalar("SELECT count(*) FROM mm_route") == 5
        assert scalar("SELECT count(DISTINCT sender) FROM mm_route") == 2
        execute(
            "ALTER TABLE mm_progress ADD CONSTRAINT interrupt_checkpoint CHECK (false)"
            if db
            else "CREATE TRIGGER interrupt_checkpoint BEFORE INSERT ON mm_progress BEGIN SELECT RAISE(FAIL,'interrupted'); END"
        )
        assert (
            subprocess.run(
                replay, env=env, cwd=root, capture_output=True, check=False
            ).returncode
            != 0
        )
        pipe = mrsm.Pipe("weather", "temperature", instance="sql:mmtest")
        assert pipe.get_docs(limit=5) == [{"tick": 1, "value": 11}]
        assert scalar("SELECT count(*) FROM mm_progress") == 0
        execute(
            "ALTER TABLE mm_progress DROP CONSTRAINT interrupt_checkpoint"
            if db
            else "DROP TRIGGER interrupt_checkpoint"
        )
        checked_command(*replay, env=env, cwd=root)
        assert pipe.get_docs(limit=5) == [{"tick": 1, "value": 44}]
        ok, msg = pipe.lock("sync")
        assert ok, msg
        assert post(v1, batch([record(3, doc(55))]), direct) == (200, expected(3))
        assert (
            subprocess.run(
                replay, env=env, cwd=root, capture_output=True, check=False
            ).returncode
            != 0
        )
        assert pipe.get_docs(limit=5) == [{"tick": 1, "value": 44}]
        other = mrsm.Pipe("weather", "second_output", instance="sql:mmtest")
        assert other.get_docs(limit=5) == [{"tick": 1, "value": 55}]
        ok, msg = pipe.unlock("sync")
        assert ok, msg
        checked_command(*replay, env=env, cwd=root)
        assert pipe.get_docs(limit=5) == [{"tick": 1, "value": 55}]
        assert post(v1, batch(), direct) == (200, expected(3))
        assert post(v1, batch([record(6, loss(2, 4, 5), kind=1)]), direct) == (
            200,
            expected(6),
        )
        assert post(v1, batch([record(7, doc(77))]), direct) == (200, expected(7))
        checked_command(*replay, env=env, cwd=root)
        assert post(v1, batch([record(4, doc(41))]), direct) == (200, expected(7))
        checked_command(*replay, env=env, cwd=root)
        assert pipe.get_docs(limit=5) == [{"tick": 1, "value": 77}]
        # A configured document parser rejects an opaque payload after ingress has acknowledged it.
        assert post(v1, batch([record(8, b"opaque")]), direct) == (200, expected(8))
        assert (
            subprocess.run(
                replay, env=env, cwd=root, capture_output=True, check=False
            ).returncode
            != 0
        )
        assert post(v1, batch(), direct) == (200, expected(8))
        assert pipe.get_docs(limit=5) == [{"tick": 1, "value": 77}]
        status = checked_command(
            "mrsm", "mm", "receiver", "status", env=env, cwd=root
        ).decode()
        assert '"durable_through":8' in status and '"applied":7' in status, status
        if not db:
            raw.close()

    if micropython:
        qualify_chip_sensor(root, micropython, config, raw_db, tokens[0])

    print(
        "Passed authenticated HTTPS capture, restart, retries, conflicts, legacy ingress and ordered locked-Pipe replay."
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument(
        "--postgres",
        action="store_true",
        help="Start a disposable PostgreSQL 18 Docker container.",
    )
    parser.add_argument(
        "--micropython", type=Path, help="Exercise the chip_sensor example with this Unix build."
    )
    args = parser.parse_args()
    with (
        tempfile.TemporaryDirectory() as directory,
        postgres() if args.postgres else contextlib.nullcontext(None) as db,
    ):
        mp = args.micropython.resolve() if args.micropython else None
        qualify(Path(directory), args.library.resolve(), db, mp)
