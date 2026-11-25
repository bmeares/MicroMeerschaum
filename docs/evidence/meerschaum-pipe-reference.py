"""Run with the Meerschaum 5.1.5 Python interpreter. Uses two temporary SQLite files."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def incremental_copy():
    result = subprocess.run(
        [
            sys.executable, "-P", "-m", "meerschaum", "copy", "pipes", "sql:mmrefdest",
            "-i", "sql:mmrefsource", "-c", "sensor", "-m", "reading",
            "--sync-data", "incremental", "-y",
        ],
        capture_output=True, text=True, timeout=60,
    )
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


def check(root):
    # Start Meerschaum with an isolated configuration for both Python and the CLI.
    for key in list(os.environ):
        if key.startswith("MRSM_"):
            del os.environ[key]
    config = root / "config"
    config.mkdir()
    os.environ.update({
        "MRSM_ROOT_DIR": str(config),
        "MRSM_SQL_MMREFSOURCE": f"sqlite:///{root / 'source.db'}",
        "MRSM_SQL_MMREFDEST": f"sqlite:///{root / 'dest.db'}",
        "MRSM_NO_AUTO_INSTALL": "1",
        "MRSM_NO_UPDATE_CHECK": "1",
        "XDG_CACHE_HOME": str(root / "cache"),
    })
    import meerschaum as mrsm

    if mrsm.__version__ != "5.1.5":
        raise RuntimeError(f"Expected Meerschaum 5.1.5, found {mrsm.__version__}")

    source = mrsm.Pipe(
        "sensor", "reading", instance="sql:mmrefsource",
        columns={"datetime": "tick", "id": "sensor_id"},
        dtypes={"tick": "int", "sensor_id": "string", "value": "int"},
        parameters={"fetch": {"backtrack_minutes": 1}},
        upsert=True,
    )
    for alias in ("None", "[None]"):
        assert mrsm.Pipe("sensor", "reading", alias, instance="sql:mmrefsource").location_key is None
    assert not (root / "source.db").exists()
    registered, message = source.register()
    assert registered, message
    assert source.get_id() is not None and not source.exists()
    assert source.get_sync_time() is None

    first = {"tick": 1, "sensor_id": "NA", "value": 11}
    rows = [
        first,
        {"tick": 2, "sensor_id": "east", "value": 22},
        {"tick": 10000, "sensor_id": "east", "value": 33},
    ]
    result = source.sync(rows)
    success, message = result
    assert success and result.success, message
    assert source.get_docs(begin=1, end=2, params={"sensor_id": ["NA", "east"]}, limit=5) == [first]
    assert source.get_sync_time() == 10000
    assert source.get_sync_time(params={"sensor_id": "NA"}) == 1

    incremental_copy()
    destination = mrsm.Pipe("sensor", "reading", instance="sql:mmrefdest")
    assert destination.get_rowcount() == 3
    corrected = dict(first, value=44)
    success, message = source.sync([corrected])
    assert success, message
    incremental_copy()
    assert source.get_docs(begin=1, end=2, limit=5) == [corrected]
    assert destination.get_docs(begin=1, end=2, limit=5) == [first]

    # SQLite's default UNIQUE handling of NULL alone would retain both null-key rows.
    for key, value in [(None, 1), (None, 2), ("None", 3)]:
        success, message = source.sync([{"tick": 3, "sensor_id": key, "value": value}])
        assert success, message
    at_three = source.get_docs(begin=3, end=4, limit=5)
    assert len(at_three) == 2
    assert {r["sensor_id"]: r["value"] for r in at_three} == {None: 2, "None": 3}

    # Simulate two arrivals for one source row, with the correction arriving first.
    arrival = mrsm.Pipe(
        "sensor", "arrival", instance="sql:mmrefdest",
        columns={"datetime": "tick", "id": "sensor_id"},
        dtypes={"tick": "int"}, upsert=True,
    )
    for row in (corrected, first):
        success, message = arrival.sync([row])
        assert success, message
    assert arrival.get_docs(limit=2) == [first]
    success, message = arrival.sync([corrected])
    assert success, message
    assert arrival.get_docs(limit=2) == [corrected]
    print("Verified registration, sync result, bounded reads, filtered sync time and nullable keys.")
    print("After incremental copy: tick 1 is 44 on the source and 11 on the destination.")
    print("Out-of-order upserts produced 11. Applying the correction last restored 44.")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:
        check(Path(directory))
