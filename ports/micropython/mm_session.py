# SPDX-License-Identifier: Apache-2.0
"""Bounded device management sessions on dedicated byte streams."""
import json
import mm_request_json
from micromeerschaum import Timestamp, default_actions

PREFIX = b"@micro "
FRAME_LIMIT = 4096
_OPTIONS = {
    "catalog": ("offset", "limit"),
    "read": ("key", "limit", "begin", "end", "params", "order"),
    "write": ("key", "rows"),
    "sync_time": ("key", "params"),
    "rowcount": ("key", "begin", "end", "params"),
    "actions": (),
    "complete": ("line",),
    "action": ("line",),
}


def _integer(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        raise ValueError("invalid " + name)
    return value


def _scalar(value):
    if isinstance(value, dict) and len(value) == 1 and "$us" in value:
        return Timestamp(_integer(value["$us"], -(1 << 63), (1 << 63) - 1, "timestamp"))
    if value is None or type(value) in (bool, int, float, str):
        return value
    raise ValueError("expected a document scalar")


def _params(params):
    if params is None:
        return None
    if not isinstance(params, dict):
        raise ValueError("expected params object")
    return {k: [_scalar(v) for v in val] if isinstance(val, list) else _scalar(val)
            for k, val in params.items()}


def _wire_doc(row):
    return {k: {"$us": v.us} if isinstance(v, Timestamp) else v for k, v in row.items()}


def _frame(ident, ok, result):
    doc = {"v": 1, "id": ident, "ok": ok}
    doc["data" if ok else "error"] = result
    return PREFIX + json.dumps(doc, separators=(",", ":")).encode() + b"\n"


def _catalog(entry, operations):
    item = {"id": entry.id, "key": entry.key, "instance": entry.pipe.instance_keys,
            "read": entry.read and "read" in operations, "write": entry.write and "write" in operations,
            "filters": ["begin", "end", "params"] if entry.pipe.instance_keys == "sql:local" else []}
    if entry.pipe.instance_keys == "sql:local":
        item["columns"] = entry.pipe.columns
        item["dtypes"] = entry.pipe.dtypes
    return item


class DeviceSession:
    """Application-granted operation and action permissions for an entry point."""
    def __init__(self, registry=default_actions, operations=(), allowed_actions=()):
        self.registry = registry
        self.operations = tuple(operations)
        self.allowed_actions = tuple(allowed_actions)
        if any(op not in _OPTIONS for op in self.operations):
            raise ValueError("unknown session operation")
        if any(name not in registry.names() for name in self.allowed_actions):
            raise ValueError("unknown session action")

    def _admitted(self, op):
        if op in self.operations:
            return True
        return op in ("sync_time", "rowcount") and "read" in self.operations

    def _catalog(self, request, ident):
        offset = _integer(request.get("offset", 0), 0, (1 << 63) - 1, "catalog offset")
        limit = _integer(request.get("limit", 16), 0, 64, "catalog limit")
        entries = self.registry.entries()
        result = {"items": [], "more": offset < len(entries)}
        if offset >= len(entries):
            return result
        for entry in entries[offset:offset + limit]:
            result["items"].append(_catalog(entry, self.operations))
            result["more"] = offset + len(result["items"]) < len(entries)
            if len(_frame(ident, True, result)) > FRAME_LIMIT:
                result["items"].pop()
                if not result["items"]:
                    raise ValueError("catalog entry exceeds frame limit")
                result["more"] = True
                break
        return result

    def _pipe(self, request, op):
        key = request.get("key")
        if not (isinstance(key, list) and len(key) == 3 and
                all(isinstance(v, str) and v for v in key[:2]) and
                (key[2] is None or isinstance(key[2], str))):
            raise ValueError("expected a Pipe key triple")
        need_write = op == "write"
        for entry in self.registry.entries():
            if entry.key == tuple(key) and (entry.write if need_write else entry.read):
                return entry.pipe
        raise ValueError("Pipe unavailable for " + op)

    def _read(self, request, pipe):
        limit = _integer(request.get("limit"), 0, 128, "read limit")
        order = request.get("order", "asc")
        if order not in ("asc", "desc"):
            raise ValueError("order must be 'asc' or 'desc'")
        begin, end, params = (request.get(k) for k in ("begin", "end", "params"))
        store = pipe._config().store
        if pipe.instance_keys == "sql:local":
            rows = store.docs(_scalar(begin), _scalar(end), _params(params),
                              limit, FRAME_LIMIT, desc=(order == "desc"))
            return [_wire_doc(row) for row in rows]
        if order != "asc":
            raise ValueError("journal reads support ascending order only")
        if begin is not None or end is not None or params not in (None, {}):
            raise ValueError("journal reads support limit only")
        rows, size = [], 0
        # Each binary field adds at most 16 bytes beyond its name and value.
        for row in pipe._retained_docs(limit, FRAME_LIMIT + 64 * 16 + 32):
            doc = _wire_doc(row)
            size += len(json.dumps(doc, separators=(",", ":")).encode())
            if size > FRAME_LIMIT:
                raise ValueError("rows exceed frame limit")
            rows.append(doc)
        return rows

    def _sync_time(self, request, pipe):
        params = request.get("params")
        if pipe.instance_keys == "sql:local":
            store = pipe._config().store
            value = store.sync_time(_params(params))
            if value is None:
                return None
            axis, cols, _ = store.definition()
            for name, dtype_name, _ in cols:
                if name == axis:
                    return {"$us": value} if dtype_name == "datetime" else value
            return value
        if params not in (None, {}):
            raise ValueError("journal sync_time does not support filters")
        return None

    def _rowcount(self, request, pipe):
        begin, end, params = (request.get(k) for k in ("begin", "end", "params"))
        if pipe.instance_keys == "sql:local":
            return pipe._config().store.rowcount(_scalar(begin), _scalar(end), _params(params))
        if begin is not None or end is not None or params not in (None, {}):
            raise ValueError("journal rowcount does not support filters")
        return pipe.get_rowcount()

    def _execute(self, request, ident):
        op = request.get("op")
        if not self._admitted(op):
            raise ValueError("operation unavailable")
        if any(k not in ("id", "v", "op") and k not in _OPTIONS[op] for k in request):
            raise ValueError("unknown request field")
        if op == "catalog":
            return self._catalog(request, ident)
        if op == "actions":
            return self.registry.names(self.allowed_actions)
        if op == "complete":
            return self.registry.complete(request["line"], self.allowed_actions)
        if op == "action":
            ok, msg = self.registry.dispatch(request["line"], self.allowed_actions)
        else:
            pipe = self._pipe(request, op)
            if op == "read":
                return self._read(request, pipe)
            if op == "sync_time":
                return self._sync_time(request, pipe)
            if op == "rowcount":
                return self._rowcount(request, pipe)
            rows = request.get("rows")
            if not isinstance(rows, list) or len(rows) > 128:
                raise ValueError("expected at most 128 rows")
            for row in rows:
                if not isinstance(row, dict) or len(row) > 64:
                    raise ValueError("expected flat documents of at most 64 fields")
                for name in row:
                    row[name] = _scalar(row[name])
            ok, msg = pipe.sync(rows)
        if not ok:
            raise ValueError(msg)
        return msg

    def handle(self, line):
        """Handle a complete frame (without LF). Ignore uncorrelated input."""
        if len(line) + 1 > FRAME_LIMIT or not line.startswith(PREFIX):
            return None
        try:
            request = mm_request_json.loads(line[len(PREFIX):])
            ident = request["id"]
            if not isinstance(ident, str) or not 1 <= len(ident) <= 32 or any(
                not ("0" <= c <= "9" or "A" <= c <= "Z" or "a" <= c <= "z") for c in ident
            ):
                return None
        except Exception:
            return None
        try:
            if type(request.get("v")) is not int or request["v"] != 1:
                raise ValueError("unsupported protocol version")
            result = self._execute(request, ident)
            reply = _frame(ident, True, result)
            if len(reply) <= FRAME_LIMIT:
                return reply
            if request["op"] == "action":
                return _frame(ident, False, "action completed, output exceeds frame limit")
            raise ValueError("result exceeds frame limit")
        except Exception as error:
            reply = _frame(ident, False, str(error))
            return reply if len(reply) <= FRAME_LIMIT else _frame(ident, False, "error exceeds frame limit")


class FrameReader:
    """One connection owns its partial frame. Overlong lines drain through LF."""
    def __init__(self):
        self.buffer = bytearray()
        self.discard = False

    def feed(self, fragment, receive):
        for c in fragment:
            if c == 10:
                data = None if self.discard else bytes(self.buffer)
                self.buffer = bytearray()
                self.discard = False
                if data is not None:
                    receive(data)
            elif not self.discard:
                if len(self.buffer) >= FRAME_LIMIT - 1:
                    self.discard = True
                    self.buffer = bytearray()
                else:
                    self.buffer.append(c)


def serve_usb(session, reader=None, writer=None):
    """Serve a dedicated USB serial byte stream, or the board's console in this call."""
    import sys
    import time
    reader = sys.stdin.buffer if reader is None else reader
    writer = sys.stdout.buffer if writer is None else writer
    frames = FrameReader()
    def receive(line):
        reply = session.handle(line)
        if reply is None:
            return
        sent = 0
        while sent < len(reply):
            count = writer.write(memoryview(reply)[sent:])
            if not count:
                raise OSError("serial write interrupted")
            sent += count
        if hasattr(writer, "flush"):
            writer.flush()
    while True:
        fragment = reader.read(1)
        if fragment is None:  # machine.UART timed out
            time.sleep_ms(1)
        elif not fragment:
            return
        else:
            frames.feed(fragment, receive)
