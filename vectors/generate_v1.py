#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate protocol-v1 examples independently of both production codecs."""
import struct
from pathlib import Path

from generate import crc, record

KEY = b"sensor\x1ftemperature\x1f"
ORIGIN = 0x0123456789abcdef
GENERATION = 0xfedcba9876543210
DESTINATION = 4321


def checked(header):
    return header + struct.pack("<I", crc(header))


def stream(key=KEY, origin=ORIGIN, generation=GENERATION):
    return checked(struct.pack("<4sB3xQQI", b"MMID", 1, origin, generation, len(key)) + key)


def receipt(role, issuer, destination=DESTINATION, through=0, generation=GENERATION):
    head = struct.pack("<4sBB2xQQQQQI", b"MMRC", 1, role, ORIGIN, generation,
                       issuer, destination, through, len(KEY))
    return checked(head + KEY)


def batch(records=(), hops=(), generation=GENERATION, count=None):
    head = struct.pack("<4sBB2xQQQI", b"MMBT", 1, len(hops), ORIGIN, generation,
                       DESTINATION, len(KEY)) + KEY
    for node, received_us, transport in hops:
        head += struct.pack("<QqB7x", node, received_us, transport)
    head += struct.pack("<I", len(records) if count is None else count)
    return checked(head) + b"".join(records)


def loss(tag, first, last):
    return struct.pack("<B7xQQ", tag, first, last)


def flip(data, offset):
    damaged = bytearray(data)
    damaged[offset] ^= 1
    return bytes(damaged)


def vectors():
    saved = stream()
    yield "stream", "saved", saved, "ok"
    yield "stream", "new_generation", stream(generation=2), "ok"
    yield "stream", "long_key", stream(key=b"a" * 197 + b"\x1fb\x1f"), "ok"
    yield "stream", "zero_generation", stream(generation=0), "bad_field"
    yield "stream", "invalid_key", stream(key=b"a\x1f\xff\x1f"), "bad_key"
    yield "stream", "oversize_key", stream(key=b"a" * 198 + b"\x1fb\x1f"), "bad_key"
    yield "stream", "reserved", flip(saved, 7), "bad_field"
    yield "stream", "corrupt", flip(saved, -1), "bad_crc"
    yield "stream", "short", saved[:-1], "truncated"
    yield "stream", "trailing", saved + bytes(1), "trailing"

    positive = receipt(1, DESTINATION, through=0xffffffffffffffff)
    yield "receipt", "destination", positive, "ok"
    yield "receipt", "custody", receipt(2, 17, through=6), "ok"
    yield "receipt", "other_destination", receipt(2, 17, destination=7654, through=6), "ok"
    yield "receipt", "empty_prefix", receipt(1, DESTINATION), "ok"
    yield "receipt", "wrong_issuer", receipt(1, 17), "bad_field"
    yield "receipt", "custody_at_destination", receipt(2, DESTINATION), "bad_field"
    yield "receipt", "wrong_role", receipt(77, 17), "bad_field"
    yield "receipt", "zero_generation", receipt(1, DESTINATION, generation=0), "bad_field"
    yield "receipt", "bad_magic", flip(positive, 0), "bad_magic"
    yield "receipt", "wrong_version", flip(positive, 4), "bad_version"
    yield "receipt", "corrupt_prefix", flip(positive, 40), "bad_crc"
    yield "receipt", "short", positive[:-1], "truncated"
    yield "receipt", "trailing", positive + bytes(1), "trailing"

    yield "loss", "missed_before_admission", loss(1, 11, 0), "ok"
    yield "loss", "evicted_interval", loss(2, 3, 7), "ok"
    yield "loss", "unknown_tag", loss(3, 3, 7), "bad_field"
    yield "loss", "invalid_count", loss(1, 0, 0), "bad_field"
    yield "loss", "missed_has_range", loss(1, 3, 1), "bad_field"
    yield "loss", "reserved", flip(loss(2, 3, 7), 1), "bad_field"
    yield "loss", "zero_sequence", loss(2, 0, 1), "bad_field"
    yield "loss", "reversed_interval", loss(2, 7, 3), "bad_field"
    yield "loss", "includes_own_sequence", loss(2, 3, 8), "bad_field"
    yield "loss", "short", loss(1, 1, 0)[:-1], "truncated"
    yield "loss", "trailing", loss(1, 1, 0) + bytes(1), "trailing"

    a = record(42, payload=b"degree:19")
    b = record(43, payload=b"degree:19")
    good = batch([a, b], [(15, -99, 1)])
    yield "batch", "two_equal_readings", good, "ok"
    yield "batch", "empty", batch(), "ok"
    yield "batch", "eight_hops", batch(hops=[(i, 0, 4) for i in range(8)]), "ok"
    yield "batch", "declared_loss", batch([record(8, loss(2, 1, 7), kind=1)]), "ok"
    yield "batch", "max_sequence", batch([record(0xffffffffffffffff)]), "ok"
    yield "batch", "bad_hop_count", batch(hops=[(1, 0, 0)] * 9), "bad_field"
    yield "batch", "bad_hop_transport", batch(hops=[(1, 0, 5)]), "bad_field"
    yield "batch", "bad_generation", batch(generation=0), "bad_field"
    yield "batch", "legacy_version", flip(good, 4), "bad_version"
    yield "batch", "bad_header_crc", flip(good, 24), "bad_crc"
    yield "batch", "gap", batch([a, record(44)]), "seq_gap"
    yield "batch", "sequence_wrap", batch([record(0xffffffffffffffff), record(1)]), "seq_gap"
    yield "batch", "bad_record_crc", flip(good, -1), "bad_crc"
    yield "batch", "includes_future_loss", batch([record(7, loss(2, 1, 8), kind=1)]), "bad_field"
    yield "batch", "missing_record", batch(count=1), "truncated"
    yield "batch", "trailing", good + b"\x00", "trailing"


if __name__ == "__main__":
    with Path(__file__).with_name("protocol-v1.txt").open("w", encoding="ascii") as out:
        print("# v1: <structure> <name> <hex> <status>. Standalone loss record sequence is 8.", file=out)
        for kind, name, data, status in vectors():
            print(kind, name, data.hex(), status, file=out)
