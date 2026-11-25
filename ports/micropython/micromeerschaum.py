# SPDX-License-Identifier: Apache-2.0
"""MicroMeerschaum device Pipes. Open storage explicitly with provision()."""
import time
import _mm
from _mm import Timestamp

_stores = {}
_peers = {}
_local = "journal:local"
_missing = object()


def _wire_key(connector, metric, location):
    if not connector or not metric:
        raise ValueError("connector and metric are required")
    parts = (connector, metric, "" if location is None else location)
    for key in parts:
        if not isinstance(key, str) or "\x1f" in key or "\x00" in key:
            raise ValueError("invalid Pipe key")
    wire = "\x1f".join(parts)
    if len(wire.encode()) > 200:
        raise ValueError("Pipe key is too long")
    return wire


class _Registration:
    def __init__(self, store, path, budget, destination, boot, retention):
        self.store = store
        self.path = path
        self.budget = budget
        self.destination = destination
        self.boot = boot
        self.retention = retention


def provision_peer(name, issuer, exchange):
    """Install the transport to an authenticated, provisioned destination."""
    if not isinstance(name, str) or not name.startswith("peer:") or len(name) == 5:
        raise ValueError("expected peer:name")
    if not callable(exchange) or not isinstance(issuer, int) or not 0 < issuer < 1 << 64:
        raise ValueError("expected an issuer and exchange(batch) callable")
    _peers[name] = (issuer, exchange)


def provision(connector, metric, path, origin, generation, destination, location=None,
              instance=_local, receiver=False, segments=16, segment_bytes=4096,
              retention="keep-old", create=False, boot=0):
    pipe = Pipe(connector, metric, location, instance)
    if not instance.startswith("journal:"):
        raise ValueError("expected a journal instance")
    if pipe._id() in _stores or any(c.path == path for c in _stores.values()):
        raise ValueError("close the Pipe before provisioning it again")
    if retention not in ("keep-old", "keep-new"):
        raise ValueError("unknown retention policy")
    store = _mm.open_volume(path, pipe._wire, origin, generation, destination, receiver,
                            segments, segment_bytes, retention == "keep-new", create)
    # The core reserves room for a sequence-bearing loss record under keep-new.
    budget = segment_bytes - 28 - len(pipe._wire.encode()) - (56 if retention == "keep-new" else 0)
    _stores[pipe._id()] = _Registration(store, path, budget, destination, boot, retention)
    return pipe


def _rows(frame):
    if isinstance(frame, dict):
        if not frame:
            return
        if len(frame) > 64:
            raise ValueError("expected at most 64 columns")
        columns = [v for v in frame.values() if isinstance(v, (tuple, list))]
        if not columns:
            yield frame
            return
        n = len(columns[0])
        if len(columns) != len(frame) or any(len(c) != n for c in columns):
            raise ValueError("expected equal-length columns")
        for i in range(n):
            yield {name: values[i] for name, values in frame.items()}
    elif hasattr(frame, "itertuples") and hasattr(frame, "columns"):
        if len(frame.columns) > 64:
            raise ValueError("expected at most 64 columns")
        names = tuple(frame.columns)
        if len(set(names)) != len(names):
            raise ValueError("duplicate frame column")
        for values in frame.itertuples(index=False, name=None):
            if len(values) != len(names):
                raise ValueError("row width differs from column count")
            yield dict(zip(names, values))
    else:
        if isinstance(frame, (str, bytes, bytearray)):
            raise ValueError("expected an iterable of row dictionaries")
        for row in frame:
            yield row


class Pipe:
    def __init__(self, connector, metric, location=None, instance=_local):
        self.connector_keys = connector
        self.metric_key = metric
        self.location_key = location
        self.instance_keys = instance
        self.parameters = {}
        self._registered = False
        self._wire = _wire_key(connector, metric, location)
        self.staged_input = 0
        self.confirmed_input = 0
        self.uncertain_first = 0
        self.uncertain_count = 0

    def _id(self):
        return (_local if self.instance_keys.startswith("peer:") else self.instance_keys, self._wire)

    def _config(self):
        config = _stores.get(self._id())
        if config is None:
            raise ValueError("Pipe has not been provisioned")
        return config

    def progress(self):
        return dict(zip(("prefix", "high", "confirmed", "missed"), self._config().store.progress()))

    def close(self):
        store = self._config().store
        store.close()
        del _stores[self._id()]

    def _source(self):
        if not self.connector_keys.startswith("plugin:") or not self.connector_keys[7:]:
            raise ValueError("supply data or select a plugin source")
        return __import__(self.connector_keys[7:], None, None, ("fetch",))

    def _register(self, module, kw):
        hook = getattr(module, "register", None)
        defaults = hook(self, **kw) if hook else None
        if defaults is not None:
            if not isinstance(defaults, dict):
                raise ValueError("register must return a dictionary or None")
            self.parameters.update(defaults)
        self._registered = True

    def register(self, **kw):
        try:
            self._register(self._source(), kw)
            return True, "registered source"
        except Exception as error:
            return False, str(error)

    def sync(self, df=_missing, **kw):
        self.staged_input = self.confirmed_input = self.uncertain_first = self.uncertain_count = 0
        remote = self.instance_keys.startswith("peer:")
        start = None
        try:
            config = self._config()
            if remote:
                self._peer(self.instance_keys)
                if config.retention != "keep-old":
                    raise ValueError("peer staging requires keep-old retention")
                start = self.progress()["high"]
            if df is _missing:
                module = self._source()
                if not self._registered:
                    self._register(module, {})
                df = module.fetch(self, **kw)
            elif kw:
                raise ValueError("source arguments require a source fetch")
            group = []
            group_bytes = 0
            budget = config.budget
            for row in _rows(df):
                if not isinstance(row, dict) or len(row) > 64:
                    raise ValueError("expected a document with at most 64 fields")
                payload = _mm.encode(list(row.items()))
                size = 32 + len(payload)
                if group and group_bytes + size > budget:
                    self._commit(config, group)
                    group = []
                    group_bytes = 0
                if size > budget:
                    raise ValueError("document exceeds segment size")
                group.append(payload)
                group_bytes += size
                if len(group) == 8:
                    self._commit(config, group)
                    group = []
                    group_bytes = 0
            if group:
                self._commit(config, group)
            if remote:
                self._deliver(self.instance_keys)
            self.confirmed_input = self.staged_input
        except Exception as error:
            if remote:
                if start is not None:
                    try:
                        self.confirmed_input = max(0, min(self.staged_input, self.progress()["confirmed"] - start))
                    except (OSError, ValueError):
                        pass
                return False, "%s (staged %d, confirmed at peer %d)" % (
                    error, self.staged_input, self.confirmed_input)
            self.confirmed_input = self.staged_input
            return False, "%s (confirmed %d input rows)" % (error, self.confirmed_input)
        return True, "committed %d input rows" % self.confirmed_input

    def _commit(self, config, group):
        status, count, first, uncertain = config.store.append(group, time.ticks_us(), config.boot)
        self.staged_input += count
        self.uncertain_first = first
        self.uncertain_count = uncertain
        if status != "ok":
            raise OSError(status)

    def read_record(self, seq):
        return self._config().store.read_record(seq)

    def read_doc(self, seq):
        record = self.read_record(seq)
        if record[3] != 0:
            raise ValueError("sequence contains a loss record")
        return _mm.decode(memoryview(record)[32:])

    def batch(self, after=None, max_records=8, cap=4096):
        store = self._config().store
        if after is None:
            after = store.progress()[2]
        return store.batch(after, max_records, cap)

    def receive(self, authenticated_origin, batch):
        return self._config().store.receive(authenticated_origin, batch)

    def confirm(self, authenticated_issuer, receipt):
        self._config().store.confirm(authenticated_issuer, receipt)

    def reclaim(self):
        self._config().store.reclaim()

    def _peer(self, name):
        peer = _peers.get(name)
        if peer is None:
            raise ValueError("peer has not been provisioned")
        if peer[0] != self._config().destination:
            raise ValueError("peer identity differs from provisioned destination")
        return peer

    def _deliver(self, name):
        issuer, exchange = self._peer(name)
        state = self.progress()
        through = state["confirmed"]
        while through < state["high"]:
            receipt = exchange(self.batch(after=through))
            self.confirm(issuer, receipt)
            next_through = self.progress()["confirmed"]
            if next_through <= through:
                raise OSError("peer receipt did not advance")
            through = next_through
        return through

    def deliver(self, peer=None):
        try:
            through = self._deliver(self.instance_keys if peer is None else peer)
            return True, "peer confirmed through %d" % through
        except Exception as error:
            return False, str(error)

    def copy_to(self, instance):
        """Copy pending original records to a compatible provisioned peer Pipe."""
        return self.deliver(instance)
