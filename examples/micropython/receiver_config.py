# SPDX-License-Identifier: Apache-2.0
"""Generate the hosted chip_sensor mapping from the board's local registration."""
import argparse
import json
import runpy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("token_id", help="public ID of the device's mm:ingest token")
    parser.add_argument("raw_store", help="writable raw SQLite or PostgreSQL connection URL")
    parser.add_argument("--identity", default="device_identity.py", help="trusted board registration")
    parser.add_argument("--instance", default="sql:local", help="ordinary Pipe's instance")
    args = parser.parse_args()
    identity = runpy.run_path(args.identity)
    source = {
        "origin": str(identity["ORIGIN"]),
        "key": "plugin:chip_sensor\x1ftemperature\x1f",
    }
    receiver = {
        "destination": str(identity["DESTINATION"]),
        "raw_store": args.raw_store,
        "grants": [dict(source, token_id=args.token_id)],
        "outputs": [dict(
            source,
            generation=str(identity["GENERATION"]),
            instance=args.instance,
            connector="weather",
            metric="chip_temperature",
            columns={"datetime": "tick"},
            dtypes={"tick": "int", "celsius": "int"},
        )],
    }
    print(json.dumps({"plugins": {"mm_receiver": receiver}}, indent=2))


if __name__ == "__main__":
    main()
