# SPDX-License-Identifier: Apache-2.0
"""MicroMeerschaum device Pipes. Open storage explicitly with provision()."""
import time
import _mm
from _mm import Timestamp

__version__ = "0.2.1"

_stores = {}
_peers = {}
_local = "journal:local"
_missing = object()


def _resolve(name, values, default=None):
    chosen = _missing
    for value in values:
        if value is None:
            continue
        if chosen is _missing:
            chosen = value
        elif chosen != value:
            raise ValueError("conflicting " + name + " arguments")
    return default if chosen is _missing else chosen


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
    def __init__(self, keys, store, path, budget, delivery_issuer, boot, retention, custody, segments):
        self.keys = keys
        self.store = store
        self.path = path
        self.budget = budget
        self.segments = segments
        self.delivery_issuer = delivery_issuer
        self.custody = custody
        self.boot = boot
        self.retention = retention


def _remote(name):
    return name.startswith("peer:") or name.startswith("api:")


def provision_peer(name, issuer, exchange, cap=4096, max_records=8):
    """Install the transport to an authenticated, provisioned destination."""
    if not isinstance(name, str) or not _remote(name) or not name.split(":", 1)[1]:
        raise ValueError("expected peer:name or api:name")
    if not callable(exchange) or not isinstance(issuer, int) or not 0 < issuer < 1 << 64:
        raise ValueError("expected an issuer and exchange(batch) callable")
    if cap < 1 or max_records < 1:
        raise ValueError("positive transfer limits required")
    _peers[name] = (issuer, exchange, cap, max_records)


def provision(connector, metric, path, origin, generation, destination, location=None,
              instance=_local, receiver=False, segments=16, segment_bytes=4096,
              retention="keep-old", create=False, boot=0, custodian=None,
              columns=None, axis=None, autotime=False):
    pipe = Pipe(connector, metric, location, instance)
    if pipe._id() in _stores or any(c.path == path for c in _stores.values()):
        raise ValueError("close the Pipe before provisioning it again")
    if retention not in ("keep-old", "keep-new"):
        raise ValueError("unknown retention policy")
    if instance == "sql:local":
        if receiver or retention != "keep-old":
            raise ValueError("sql:local requires an originating Pipe with keep-old retention")
        import _mmsqlite
        store = _mmsqlite.open(path, pipe._wire, origin, generation, destination,
                              columns, axis, autotime, create, custodian)
        budget = segment_bytes
    elif instance.startswith("journal:"):
        if columns is not None or axis is not None or autotime:
            raise ValueError("row registration requires sql:local")
        store = _mm.open_volume(path, pipe._wire, origin, generation, destination, receiver,
                                segments, segment_bytes, retention == "keep-new", create, custodian)
        # A keep-new segment also reserves space for the eviction declaration.
        budget = segment_bytes - 28 - len(pipe._wire.encode()) - (56 if retention == "keep-new" else 0)
    else:
        raise ValueError("unknown storage instance")
    issuer = custodian if custodian is not None and not receiver else destination
    _stores[pipe._id()] = _Registration(
        (pipe.connector_keys, pipe.metric_key, pipe.location_key, pipe.instance_keys),
        store, path, budget, issuer, boot, retention,
        custodian is not None and not receiver, segments)
    return pipe


def _match_token(value, token, is_location):
    if token == "*":
        return True
    if is_location:
        if token is None or token == "[None]":
            return value is None
        if token == "None":
            return value is None or value == "None"
    return value == token


def _matches_filter(value, selector, is_location=False):
    if selector is None:
        return True
    items = selector if isinstance(selector, (list, tuple, set)) else (selector,)
    includes, excludes = [], []
    for item in items:
        if isinstance(item, str) and item.startswith("_"):
            excludes.append(item[1:])
        else:
            includes.append(item)
    if includes and not any(_match_token(value, token, is_location) for token in includes):
        return False
    if excludes and any(_match_token(value, token, is_location) for token in excludes):
        return False
    return True


def get_pipes(connector_keys=None, metric_keys=None, location_keys=None,
              instance_keys=None, as_list=False, mrsm_instance=None, instance=None):
    """Return currently provisioned Pipes as {connector: {metric: {location: Pipe}}} or a list."""
    inst_filter = _resolve("instance", (instance_keys, mrsm_instance, instance))
    pipes = []
    for reg in _stores.values():
        ck, mk, lk, ik = reg.keys
        if not (_matches_filter(ck, connector_keys)
                and _matches_filter(mk, metric_keys)
                and _matches_filter(lk, location_keys, is_location=True)
                and _matches_filter(ik, inst_filter)):
            continue
        pipes.append(Pipe(ck, mk, lk, instance=ik))
    if as_list:
        return pipes
    nested = {}
    for pipe in pipes:
        metrics = nested.setdefault(pipe.connector_keys, {})
        locations = metrics.setdefault(pipe.metric_key, {})
        locations[pipe.location_key] = pipe
    return nested


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


def _groups(frame, budget):
    group = []
    group_bytes = 0
    for row in _rows(frame):
        if not isinstance(row, dict) or len(row) > 64:
            raise ValueError("expected a document with at most 64 fields")
        payload = _mm.encode(list(row.items()))
        size = 32 + len(payload)
        if group and group_bytes + size > budget:
            yield group
            group = []
            group_bytes = 0
        if size > budget:
            raise ValueError("document exceeds segment size")
        group.append(payload)
        group_bytes += size
        if len(group) == 8:
            yield group
            group = []
            group_bytes = 0
    if group:
        yield group


class Pipe:
    def __init__(self, connector=None, metric=None, location=None, instance=None,
                 connector_keys=None, metric_key=None, location_key=None,
                 instance_keys=None, mrsm_instance=None):
        connector = _resolve("connector", (connector, connector_keys))
        metric = _resolve("metric", (metric, metric_key))
        location = _resolve("location", (location, location_key))
        instance = _resolve("instance", (instance, instance_keys, mrsm_instance), _local)
        if not isinstance(instance, str) or not instance:
            raise ValueError("invalid Pipe instance")
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

    def _key_tuple(self):
        return (self.connector_keys, self.metric_key, self.location_key, self.instance_keys)

    def __repr__(self):
        parts = [repr(self.connector_keys), repr(self.metric_key)]
        if self.location_key is not None:
            parts.append(repr(self.location_key))
        if self.instance_keys != _local:
            parts.append("instance=" + repr(self.instance_keys))
        return "Pipe(" + ", ".join(parts) + ")"

    __str__ = __repr__

    def __eq__(self, other):
        return isinstance(other, Pipe) and self._key_tuple() == other._key_tuple()

    def __hash__(self):
        return hash(self._key_tuple())

    def _id(self):
        return (_local if _remote(self.instance_keys) else self.instance_keys, self._wire)

    def _config(self):
        config = _stores.get(self._id())
        if config is None:
            raise ValueError("Pipe has not been provisioned")
        return config

    def exists(self):
        return self._id() in _stores

    def progress(self):
        return dict(zip(("prefix", "high", "confirmed", "missed"), self._config().store.progress()))

    def show(self):
        """Print a summary of this Pipe's identity, instance, parameters, and progress."""
        opened = self.exists()
        lines = [
            repr(self),
            "  instance: " + self.instance_keys,
            "  exists: " + str(opened),
            "  parameters: " + repr(self.parameters),
        ]
        if opened:
            state = self.progress()
            lines.append(
                "  progress: prefix=%d high=%d confirmed=%d missed=%d" % (
                    state["prefix"], state["high"], state["confirmed"], state["missed"]
                )
            )
        print("\n".join(lines))
        return True, "Success"

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

    def fetch(self, **kw):
        module = self._source()
        if not self._registered:
            self._register(module, {})
        return module.fetch(self, **kw)

    def sync(self, df=_missing, **kw):
        self.staged_input = self.confirmed_input = self.uncertain_first = self.uncertain_count = 0
        remote = _remote(self.instance_keys)
        try:
            config = self._config()
            if remote:
                self._peer(self.instance_keys)
                if config.custody:
                    raise ValueError("direct sync requires a final destination")
                if config.retention != "keep-old":
                    raise ValueError("peer staging requires keep-old retention")
            if df is _missing:
                df = self.fetch(**kw)
            elif kw:
                raise ValueError("source arguments require a source fetch")
            groups = _groups(df, config.budget)
            if remote:
                self._sync_remote(config, groups)
            else:
                for group in groups:
                    self._commit(config, group)
            self.confirmed_input = self.staged_input
        except Exception as error:
            if remote:
                return False, "%s (staged %d, confirmed at peer %d)" % (
                    error, self.staged_input, self.confirmed_input)
            self.confirmed_input = self.staged_input
            return False, "%s (confirmed %d input row%s)" % (
                error, self.confirmed_input, "" if self.confirmed_input == 1 else "s")
        return True, "committed %d input row%s" % (
            self.confirmed_input, "" if self.confirmed_input == 1 else "s")

    def _sync_remote(self, config, groups):
        delivery_error = None
        self.reclaim()  # Recover space after a reset between confirmation and reclamation.
        state = self.progress()
        # Each occupied segment retains at least one pending sequence.
        # When the journal could be full, drain before recording a miss.
        if state["high"] - state["confirmed"] >= config.segments:
            try:
                self._deliver(self.instance_keys)
            except Exception as error:
                delivery_error = error
        for group in groups:
            before = self.progress()["high"] if delivery_error is None else None
            self._commit(config, group)
            if before is not None:
                try:
                    self._deliver(self.instance_keys)
                    self.confirmed_input += len(group)
                except Exception as error:
                    # Keep staging while offline. A lost reply can confirm
                    # only part of this group, even if the peer stored more.
                    delivery_error = error
                    confirmed = self.progress()["confirmed"]
                    self.confirmed_input += max(0, min(len(group), confirmed - before))
        if delivery_error is not None:
            raise delivery_error
        if not self.staged_input:
            self._deliver(self.instance_keys)

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

    def receive(self, authenticated_sender, batch):
        return self._config().store.receive(authenticated_sender, batch)

    def routes(self):
        """Return (first, last, authenticated sender, reported hops) for each observed route."""
        return self._config().store.routes()

    def confirm(self, authenticated_issuer, receipt):
        self._config().store.confirm(authenticated_issuer, receipt)

    def reclaim(self):
        if self.instance_keys != "sql:local":
            self._config().store.reclaim()

    def check_delivery_issuer(self, issuer):
        """Check an authenticated node before handing it this Pipe's records."""
        if issuer != self._config().delivery_issuer:
            raise ValueError("peer differs from provisioned delivery issuer")

    def _peer(self, name):
        peer = _peers.get(name)
        if peer is None:
            raise ValueError("peer has not been provisioned")
        self.check_delivery_issuer(peer[0])
        return peer

    def _deliver(self, name):
        issuer, exchange, cap, max_records = self._peer(name)
        staging = _remote(self.instance_keys)
        state = self.progress()
        through = state["confirmed"]
        while through < state["high"]:
            receipt = exchange(self.batch(after=through, cap=cap, max_records=max_records))
            self.confirm(issuer, receipt)
            if staging:
                self.reclaim()
            state = self.progress()  # Reclamation can append a pending loss declaration.
            next_through = state["confirmed"]
            if next_through <= through:
                raise OSError("peer receipt did not advance")
            through = next_through
        return through

    def deliver(self, peer=None):
        try:
            through = self._deliver(self.instance_keys if peer is None else peer)
            role = "custodian" if self._config().custody else "destination"
            return True, "%s confirmed through %d" % (role, through)
        except Exception as error:
            return False, str(error)

    def copy_to(self, instance):
        """Copy pending original records to a compatible provisioned peer Pipe."""
        return self.deliver(instance)

    def _retained_docs(self, limit, record_limit):
        store = self._config().store
        seq = 1
        while limit and seq < 1 << 64:
            item = store.next_record(seq, record_limit)
            if item is None:
                return
            seq, record = item
            seq += 1
            if record[3] == 0:
                yield _mm.decode(memoryview(record)[32:])
                limit -= 1

    def get_docs(self, begin=None, end=None, params=None, limit=100):
        """Read current SQLite rows or the oldest retained journal documents."""
        if self.instance_keys == "sql:local":
            return self._config().store.docs(begin, end, params, limit)
        if type(limit) is not int or not 0 <= limit <= 1024:
            raise ValueError("invalid read limit")
        if begin is not None or end is not None or params not in (None, {}):
            raise ValueError("journal reads support limit only")
        return list(self._retained_docs(limit, 1 << 20))

    def get_doc(self, begin=None, end=None, params=None):
        """Read a single document, or None when no matching documents exist."""
        docs = self.get_docs(begin=begin, end=end, params=params, limit=1)
        return docs[0] if docs else None

    def get_sync_time(self, params=None):
        """Latest matching SQLite axis value, as integer ticks or Unix microseconds."""
        return self._config().store.sync_time(params)


from mm_actions import ActionRegistry, default_actions, make_action, expose, unexpose, dispatch, complete, shell
entry = dispatch
