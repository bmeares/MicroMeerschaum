#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write the shared vectors from an independent implementation of spec/wire-format.md."""

import struct
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent
MAX_PAYLOAD = 65536
MAX_KEY = 200
MAX_HOPS = 8
TRANSPORTS = 5
KEY = b"plugin:walter\x1fradar\x1fswamp-rabbit-12"
MAC = 0x30_30_F9_7A_2B_C4


class Invalid(Exception):
    pass


def crc(*chunks):
    value = 0
    for chunk in chunks:
        value = zlib.crc32(chunk, value)
    return value


# Encoders write whatever they are given, so vectors can break one rule at a time.

def record(seq, payload=b"", kind=0, mono_us=0, boot=1, magic=b"MR", version=0, length=None):
    length = len(payload) if length is None else length
    head = magic + bytes([version, kind]) + struct.pack("<IQQI", length, seq, mono_us, boot)
    return head + struct.pack("<I", crc(head, payload)) + payload


def segment_header(key=KEY, first_seq=1, boot=1, magic=b"MMSG", version=0, reserved=bytes(3), key_len=None):
    key_len = len(key) if key_len is None else key_len
    head = magic + bytes([version]) + reserved + struct.pack("<QII", first_seq, boot, key_len) + key
    return head + struct.pack("<I", crc(head))


def batch_header(hops, count, key=KEY, origin=MAC, magic=b"MMBT", version=0, hop_count=None, reserved=bytes(2)):
    hop_count = len(hops) if hop_count is None else hop_count
    head = magic + bytes([version, hop_count]) + reserved + struct.pack("<QI", origin, len(key)) + key
    for node, received_us, transport, *rest in hops:
        head += struct.pack("<QqB", node, received_us, transport) + (rest[0] if rest else bytes(7))
    head += struct.pack("<I", count)
    return head + struct.pack("<I", crc(head))


def records(first, count, boot=1, start_us=1_000_000):
    out = b""
    for i in range(count):
        payload = bytes((first + i + j) % 251 for j in range(5 + 7 * (i % 4)))
        out += record(first + i, payload, mono_us=start_us + 50_000 * i, boot=boot)
    return out


# Decoders follow the spec's check order.

def need(buf, size):
    if len(buf) < size:
        raise Invalid("truncated")


def check_key(key):
    try:
        parts = key.decode("utf-8").split("\x1f")
    except UnicodeDecodeError:
        raise Invalid("bad_key") from None
    if len(parts) != 3 or not parts[0] or not parts[1]:
        raise Invalid("bad_key")


def decode_record(buf):
    need(buf, 32)
    if buf[0:2] != b"MR":
        raise Invalid("bad_magic")
    if buf[2] != 0:
        raise Invalid("bad_version")
    if buf[3] > 1:
        raise Invalid("bad_kind")
    length, seq, mono_us, boot, want = struct.unpack_from("<IQQII", buf, 4)
    if length > MAX_PAYLOAD:
        raise Invalid("bad_length")
    need(buf, 32 + length)
    if crc(buf[:28], buf[32:32 + length]) != want:
        raise Invalid("bad_crc")
    if seq == 0:
        raise Invalid("bad_seq")
    return (32 + length, buf[3], seq, mono_us, boot, length)


def decode_segment(buf):
    need(buf, 24)
    if buf[0:4] != b"MMSG":
        raise Invalid("bad_magic")
    if buf[4] != 0:
        raise Invalid("bad_version")
    if buf[5:8] != bytes(3):
        raise Invalid("bad_field")
    first_seq, boot, key_len = struct.unpack_from("<QII", buf, 8)
    if not 1 <= key_len <= MAX_KEY:
        raise Invalid("bad_key")
    size = 28 + key_len
    need(buf, size)
    if crc(buf[:size - 4]) != struct.unpack_from("<I", buf, size - 4)[0]:
        raise Invalid("bad_crc")
    if first_seq == 0:
        raise Invalid("bad_seq")
    key = buf[24:24 + key_len]
    check_key(key)
    offset, count, stop = size, 0, "end"
    while offset < len(buf):
        try:
            used, _, seq, _, _, _ = decode_record(buf[offset:])
        except Invalid as e:
            stop = str(e)
            break
        if seq != first_seq + count:
            stop = "seq_gap"
            break
        offset += used
        count += 1
    return (size, key.hex(), first_seq, boot, count, first_seq + count - 1, offset, stop)


def decode_batch(buf):
    need(buf, 20)
    if buf[0:4] != b"MMBT":
        raise Invalid("bad_magic")
    if buf[4] != 0:
        raise Invalid("bad_version")
    hop_count = buf[5]
    if hop_count > MAX_HOPS:
        raise Invalid("bad_field")
    if buf[6:8] != bytes(2):
        raise Invalid("bad_field")
    origin, key_len = struct.unpack_from("<QI", buf, 8)
    if not 1 <= key_len <= MAX_KEY:
        raise Invalid("bad_key")
    size = 28 + key_len + 24 * hop_count
    need(buf, size)
    if crc(buf[:size - 4]) != struct.unpack_from("<I", buf, size - 4)[0]:
        raise Invalid("bad_crc")
    key = buf[20:20 + key_len]
    check_key(key)
    hops = []
    for i in range(hop_count):
        at = 20 + key_len + 24 * i
        node, received_us, transport = struct.unpack_from("<QqB", buf, at)
        if transport >= TRANSPORTS:
            raise Invalid("bad_field")
        if buf[at + 17:at + 24] != bytes(7):
            raise Invalid("bad_field")
        hops.append(f"{node}:{received_us}:{transport}")
    (count,) = struct.unpack_from("<I", buf, size - 8)
    offset, first, last = size, 0, 0
    for i in range(count):
        used, _, seq, _, _, _ = decode_record(buf[offset:])
        if i and seq != last + 1:
            raise Invalid("seq_gap")
        first = seq if i == 0 else first
        last = seq
        offset += used
    if offset != len(buf):
        raise Invalid("trailing")
    return (size, origin, key.hex(), ",".join(hops) or "-", count, first, last)


def write(name, header, cases, decode):
    lines = [f"# {line}" for line in header]
    lines.append("# Generated by vectors/generate.py; do not edit.")
    for case, data, want in cases:
        try:
            got = ["ok", *decode(data)]
        except Invalid as e:
            got = [str(e)]
        intended = want.split()
        if [got[0], str(got[-1])][:len(intended)] != intended:
            raise SystemExit(f"{name} {case}: intended {want}, decoded {got}")
        lines.append(" ".join([case, data.hex() or "-", *map(str, got)]))
    (OUT / name).write_text("\n".join(lines) + "\n")


def flip(data, at, mask=0x01):
    return data[:at] + bytes([data[at] ^ mask]) + data[at + 1:]


def main():
    crc_inputs = [b"", b"a", b"abc", b"123456789", b"The quick brown fox jumps over the lazy dog",
                  bytes(32), bytes(range(256)), b"\xff" * 16]
    (OUT / "crc32.txt").write_text(
        "# CRC-32/ISO-HDLC vectors: <input hex or - for empty> <crc32 hex>\n"
        "# Generated by vectors/generate.py; do not edit.\n"
        + "".join(f"{data.hex() or '-'} {zlib.crc32(data):08x}\n" for data in crc_inputs))

    frame = bytes.fromhex("aaff030000000000000000000000000000000000000000000000000055cc")
    small = record(1, frame, mono_us=1_234_567, boot=3)
    write("records.txt", [
        "Record vectors: <name> <bytes> <status>",
        "ok lines continue: <consumed> <kind> <seq> <mono_us> <boot> <payload length>",
    ], [
        ("data_frame", small, "ok"),
        ("loss_json", record(42, b'{"uart_overflow":1}', kind=1, mono_us=9_000_000, boot=3), "ok"),
        ("empty_payload", record(7, mono_us=5, boot=0), "ok"),
        ("max_payload", record(2**32 + 5, bytes(i * 31 % 256 for i in range(MAX_PAYLOAD)), mono_us=2**40), "ok"),
        ("max_fields", record(2**64 - 1, b"\x00", mono_us=2**64 - 1, boot=2**32 - 1), "ok"),
        ("then_erased", small + b"\xff" * 8, "ok"),
        ("empty_input", b"", "truncated"),
        ("short_header", small[:31], "truncated"),
        ("short_payload", small[:-1], "truncated"),
        ("bad_magic", record(1, frame, magic=b"MS"), "bad_magic"),
        ("bad_version", record(1, frame, version=1), "bad_version"),
        ("bad_kind", record(1, frame, kind=2), "bad_kind"),
        ("oversize", record(1, length=MAX_PAYLOAD + 1), "bad_length"),
        ("payload_bit", flip(small, 40), "bad_crc"),
        ("header_bit", flip(small, 17), "bad_crc"),
        ("crc_bit", flip(small, 28, 0x80), "bad_crc"),
        ("seq_zero", record(0, frame), "bad_seq"),
        ("erased_ff", b"\xff" * 32, "bad_magic"),
        ("erased_00", bytes(32), "bad_magic"),
    ], decode_record)

    head = segment_header(first_seq=1, boot=4)
    body = records(1, 3, boot=4)
    torn = record(4, bytes(range(40)), mono_us=2_000_000, boot=4)
    write("segments.txt", [
        "Segment vectors: <name> <bytes> <status of the header>",
        "ok lines continue: <header size> <key> <first_seq> <boot> <records> <last seq> <frontier> <stop>",
    ], [
        ("clean", head + body, "ok end"),
        ("header_only", head, "ok end"),
        ("resumed", segment_header(b"plugin:walter\x1fpeople\x1f", first_seq=1000, boot=9)
         + records(1000, 2, boot=9) + record(1002, b'{"uart_overflow":3}', kind=1, boot=9), "ok end"),
        ("torn_header", head + body + torn[:20], "ok truncated"),
        ("torn_payload", head + body + torn[:-5], "ok truncated"),
        ("torn_then_ff", head + body + torn[:-5] + b"\xff" * 64, "ok bad_crc"),
        ("tail_ff", head + body + b"\xff" * 64, "ok bad_magic"),
        ("tail_00", head + body + bytes(64), "ok bad_magic"),
        ("short_tail_ff", head + body + b"\xff" * 7, "ok truncated"),
        ("seq_gap", head + records(1, 2, boot=4) + records(4, 2, boot=4), "ok seq_gap"),
        ("first_mismatch", segment_header(first_seq=5) + records(6, 2), "ok seq_gap"),
        ("middle_bit", head + flip(body, 37 + 20), "ok bad_crc"),
        ("multibyte_key", segment_header("plugin:walter\x1frádar\x1f🐇".encode()) + records(1, 1), "ok end"),
        ("max_key", segment_header(b"c\x1fm\x1f" + b"l" * 196), "ok end"),
        ("short_fixed", head[:23], "truncated"),
        ("short_key", head[:30], "truncated"),
        ("bad_magic", segment_header(magic=b"MMSX"), "bad_magic"),
        ("bad_version", segment_header(version=1), "bad_version"),
        ("reserved", segment_header(reserved=b"\x00\x01\x00"), "bad_field"),
        ("key_len_0", segment_header(key=b""), "bad_key"),
        ("key_len_201", segment_header(key=b"c\x1fm\x1f" + b"l" * 197), "bad_key"),
        ("header_bit", flip(head, 9), "bad_crc"),
        ("first_seq_0", segment_header(first_seq=0), "bad_seq"),
        ("key_one_sep", segment_header(key=b"plugin:walter\x1fradar"), "bad_key"),
        ("key_three_seps", segment_header(key=b"plugin:walter\x1fradar\x1fa\x1fb"), "bad_key"),
        ("key_no_metric", segment_header(key=b"plugin:walter\x1f\x1fsite"), "bad_key"),
        ("key_no_connector", segment_header(key=b"\x1fradar\x1fsite"), "bad_key"),
        ("key_overlong", segment_header(key=b"plugin\xc0\xaf\x1fradar\x1f"), "bad_key"),
        ("key_surrogate", segment_header(key=b"plugin\xed\xa0\x80\x1fradar\x1f"), "bad_key"),
        ("key_cut_utf8", segment_header(key=b"plugin\x1fradar\x1fcaf\xc3"), "bad_key"),
    ], decode_segment)

    hops2 = [(0x30_30_F9_7A_2B_C5, 1_793_000_000_123_456, 1), (0x0242AC110002, 1_793_000_001_000_000, 2)]
    hops8 = [(i + 1, -i, i % TRANSPORTS) for i in range(MAX_HOPS)]
    three = records(17, 3)
    plain = batch_header([], 3) + three
    write("batches.txt", [
        "Batch vectors: <name> <bytes> <status of the whole batch>",
        "ok lines continue: <header size> <origin> <key> <hops> <N> <first seq> <last seq>",
    ], [
        ("no_hops", plain, "ok"),
        ("two_hops", batch_header(hops2, 2) + records(500, 2, boot=7), "ok"),
        ("eight_hops", batch_header(hops8, 1) + records(1, 1), "ok"),
        ("empty", batch_header(hops2[:1], 0), "ok"),
        ("short_fixed", plain[:19], "truncated"),
        ("short_header", batch_header(hops2, 2)[:-1], "truncated"),
        ("short_records", batch_header([], 3) + three[:-1], "truncated"),
        ("missing_record", batch_header([], 4) + three, "truncated"),
        ("bad_magic", batch_header([], 0, magic=b"MMBX"), "bad_magic"),
        ("bad_version", batch_header([], 0, version=1), "bad_version"),
        ("nine_hops", batch_header(hops8 + hops8[:1], 0), "bad_field"),
        ("reserved", batch_header([], 0, reserved=b"\x00\x80"), "bad_field"),
        ("key_len_0", batch_header([], 0, key=b""), "bad_key"),
        ("header_crc", flip(plain, 10), "bad_crc"),
        ("hop_count_wrong", batch_header(hops2, 2, hop_count=1) + records(500, 2), "bad_crc"),
        ("bad_key", batch_header([], 0, key=b"no-separators"), "bad_key"),
        ("transport_5", batch_header([(1, 2, 5)], 0), "bad_field"),
        ("hop_reserved", batch_header([(1, 2, 3, b"\x00" * 6 + b"\x01")], 0), "bad_field"),
        ("record_crc", batch_header([], 3) + flip(three, 70), "bad_crc"),
        ("record_gap", batch_header([], 3) + records(17, 2) + records(20, 1), "seq_gap"),
        ("record_repeat", batch_header([], 2) + records(17, 1) + records(17, 1), "seq_gap"),
        ("record_seq_0", batch_header([], 1) + record(0, b"x"), "bad_seq"),
        ("trailing", plain + b"\x00", "trailing"),
    ], decode_batch)


if __name__ == "__main__":
    main()
