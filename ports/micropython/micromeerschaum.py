# SPDX-License-Identifier: Apache-2.0
"""MicroMeerschaum device Pipes. Open storage explicitly with provision()."""
import time
import _mm
from _mm import Timestamp

_stores = {}
_local = "journal:local"
_missing = object()


def _wire_key(connector, metric, location):
    parts = (connector, metric, "" if location is None else location)
    for key in parts:
        if not isinstance(key, str) or "\x1f" in key or "\x00" in key:
            raise ValueError("invalid Pipe key")
    return "\x1f".join(parts)


def provision(connector, metric, path, origin, generation, destination, location=None,
              instance=_local, receiver=False, segments=16, segment_bytes=4096,
              retention="keep-old", create=False, boot=0):
    pipe = Pipe(connector, metric, location, instance)
    if pipe._id() in _stores:
        raise ValueError("close the Pipe before provisioning it again")
    if retention not in ("keep-old", "keep-new"):
        raise ValueError("unknown retention policy")
    store = _mm.open_volume(path, pipe._wire, origin, generation, destination, receiver,
                            segments, segment_bytes, retention == "keep-new", create)
    _stores[pipe._id()] = (store, segment_bytes, destination, boot)
    return pipe


def _rows(frame):
    if isinstance(frame, dict):
        if not frame:
            return
        columns = [v for v in frame.values() if isinstance(v, (tuple, list))]
        if not columns:
            yield frame
            return
        n = len(columns[0])
        if len(columns) != len(frame) or any(len(c) != n for c in columns):
            raise ValueError("expected equal-length columns")
        for i in range(n):
            yield {name: values[i] for name, values in frame.items()}
    else:
        for row in frame:
            yield row


class Pipe:
    def __init__(self, connector, metric, location=None, instance=_local):
        self.connector_keys = connector
        self.metric_key = metric
        self.location_key = location
        self.instance_keys = instance
        self._wire = _wire_key(connector, metric, location)
        self.confirmed_input = 0
        self.uncertain_first = 0
        self.uncertain_count = 0

    def _id(self):
        return (self.instance_keys, self._wire)

    def _config(self):
        config = _stores.get(self._id())
        if config is None:
            raise ValueError("Pipe has not been provisioned")
        return config

    def progress(self):
        return dict(zip(("prefix", "high", "confirmed", "missed"), self._config()[0].progress()))

    def close(self):
        store = self._config()[0]
        store.close()
        del _stores[self._id()]

    def sync(self, df=_missing, **kw):
        self.confirmed_input = self.uncertain_first = self.uncertain_count = 0
        try:
            store, segment_bytes, _, boot = self._config()
            group = []
            group_bytes = 0
            budget = segment_bytes - 28 - len(self._wire.encode())
            for row in _rows(df):
                if not isinstance(row, dict) or len(row) > 64:
                    raise ValueError("expected a document with at most 64 fields")
                payload = _mm.encode(list(row.items()))
                size = 32 + len(payload)
                if group and group_bytes + size > budget:
                    self._commit(store, group, boot)
                    group = []
                    group_bytes = 0
                if size > budget:
                    raise ValueError("document exceeds segment size")
                group.append(payload)
                group_bytes += size
                if len(group) == 8:
                    self._commit(store, group, boot)
                    group = []
                    group_bytes = 0
            if group:
                self._commit(store, group, boot)
        except Exception as error:
            return False, "%s; confirmed %d input rows" % (error, self.confirmed_input)
        return True, "committed %d input rows" % self.confirmed_input

    def _commit(self, store, group, boot):
        status, count, first, uncertain = store.append(group, time.ticks_us(), boot)
        self.confirmed_input += count
        self.uncertain_first = first
        self.uncertain_count = uncertain
        if status != "ok":
            raise OSError(status)

    def read_record(self, seq):
        return self._config()[0].read_record(seq)

    def read_doc(self, seq):
        record = self.read_record(seq)
        if record[3] != 0:
            raise ValueError("sequence contains a loss record")
        return _mm.decode(memoryview(record)[32:])
