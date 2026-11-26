# SPDX-License-Identifier: Apache-2.0
"""Named device actions and an interactive prompt for the MicroPython REPL."""

_PROMPT = "[ journal:local ] > "
MAX_LINE = 4224
_DATA_DEFAULT_LIMIT = 10
_DATA_MAX_LIMIT = 128
_DATA_REPLY_LIMIT = 3500
_CONNECTOR_TYPES = ("journal", "sql", "peer", "api")
_DAYS_IN_MONTH = (0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)
_FLAGS = {"-c": 0, "-m": 1, "-l": 2, "-i": 3}
_ALIASES = {"--connector-keys": "-c", "--metric-keys": "-m", "--location-keys": "-l",
            "--mrsm-instance": "-i", "--instance": "-i", "--target-instance": "-I"}
_BUILTINS = (
    "help",
    "show actions",
    "show pipes",
    "show data",
    "show columns",
    "show rowcounts",
    "show progress",
    "show connectors",
    "show version",
    "sync pipes",
    "copy pipes",
)
_BUILTIN_ALIASES = {
    "show actions": "show action",
    "show pipes": "show pipe",
    "show data": "show datum",
    "show columns": "show column",
    "show rowcounts": "show rowcount",
    "show connectors": "show connector",
    "sync pipes": "sync pipe",
    "copy pipes": "copy pipe",
}
_BUILTIN_HELP = {
    "help": "help [ACTION]",
    "show actions": "List available actions.",
    "show pipes": "List matching configured Pipes.",
    "show data": "show data [-c CONNECTOR] [-m METRIC] [-l LOCATION] [-i INSTANCE] [--limit N] [--begin VAL] [--end VAL]",
    "show columns": "Show declared columns and types for matching Pipes.",
    "show rowcounts": "Show local row counts for matching Pipes.",
    "show progress": "Show delivery cursor state for matching Pipes.",
    "show connectors": "List active local storage instances and provisioned destinations.",
    "show version": "Show MicroMeerschaum version and runtime.",
    "sync pipes": "Fetch and commit rows from matching source Pipes.",
    "copy pipes": "copy pipes [-I] DESTINATION [-c CONNECTOR] [-m METRIC] [-l LOCATION] [-i INSTANCE]",
}


def _words(line):
    if not isinstance(line, str) or len(line.encode()) > MAX_LINE:
        raise ValueError("expected a bounded action line")
    if any(ord(c) < 32 and c != "\t" or ord(c) == 127 for c in line):
        raise ValueError("expected one action line")
    return [word for word in line.replace("\t", " ").split(" ") if word]


def _arguments(tail):
    words, word, quote = [], "", None
    started = escaped = False
    for c in tail:
        if escaped:
            word += c
            escaped = False
        elif c == "\\" and quote != "'":
            escaped = started = True
        elif quote:
            if c == quote:
                quote = None
            else:
                word += c
        elif c in ("'", '"'):
            quote, started = c, True
        elif c in (" ", "\t"):
            if started:
                words.append(word)
                word, started = "", False
        else:
            word += c
            started = True
    if quote or escaped:
        raise ValueError("unfinished quote or escape")
    if started:
        words.append(word)
    if len(words) > 32:
        raise ValueError("too many action arguments")
    return words


def _is_destination(word):
    return word.startswith(("api:", "peer:")) and bool(word.split(":", 1)[1])


def _digits(text, length=None):
    if not text or (length is not None and len(text) != length):
        return False
    for c in text:
        if not ("0" <= c <= "9"):
            return False
    return True


def _is_bound_token(word):
    if not word:
        return False
    if word.startswith("-"):
        return len(word) >= 2 and "0" <= word[1] <= "9"
    return True


def _parse_int_str(text):
    if not text:
        raise ValueError("expected an integer bound")
    body = text[1:] if text[0] in ("+", "-") else text
    if not _digits(body):
        raise ValueError("invalid integer bound " + text)
    value = int(text)
    if value < -(1 << 63) or value > (1 << 63) - 1:
        raise ValueError("integer bound out of range")
    return value


def _parse_iso_us(text):
    s = text
    if s.endswith("Z") or s.endswith("z"):
        s = s[:-1]
    elif s.endswith("+00:00") or s.endswith("-00:00"):
        s = s[:-6]
    if len(s) == 10:
        date_part, time_part = s, None
    elif len(s) >= 19 and s[10] in ("T", "t", " "):
        date_part, time_part = s[:10], s[11:]
    else:
        raise ValueError("invalid datetime bound " + text)
    if date_part[4] != "-" or date_part[7] != "-":
        raise ValueError("invalid datetime bound " + text)
    ys, ms, ds = date_part[:4], date_part[5:7], date_part[8:10]
    if not (_digits(ys, 4) and _digits(ms, 2) and _digits(ds, 2)):
        raise ValueError("invalid datetime bound " + text)
    year, month, day = int(ys), int(ms), int(ds)
    if not 1 <= month <= 12:
        raise ValueError("invalid datetime bound " + text)
    leap = year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)
    max_day = 29 if month == 2 and leap else _DAYS_IN_MONTH[month]
    if not 1 <= day <= max_day:
        raise ValueError("invalid datetime bound " + text)
    hour = minute = second = usec = 0
    if time_part is not None:
        if "." in time_part:
            hms, frac = time_part.split(".", 1)
            if not _digits(frac) or len(frac) > 6:
                raise ValueError("invalid datetime bound " + text)
            usec = int(frac + "0" * (6 - len(frac)))
        else:
            hms = time_part
        if len(hms) != 8 or hms[2] != ":" or hms[5] != ":":
            raise ValueError("invalid datetime bound " + text)
        hs, mins, ss = hms[:2], hms[3:5], hms[6:8]
        if not (_digits(hs, 2) and _digits(mins, 2) and _digits(ss, 2)):
            raise ValueError("invalid datetime bound " + text)
        hour, minute, second = int(hs), int(mins), int(ss)
        if not (0 <= hour <= 23 and 0 <= minute <= 59 and 0 <= second <= 59):
            raise ValueError("invalid datetime bound " + text)
    y = year - (1 if month <= 2 else 0)
    era = y // 400
    yoe = y - era * 400
    doy = (153 * (month + (-3 if month > 2 else 9)) + 2) // 5 + day - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    days = era * 146097 + doe - 719468
    return (((days * 86400) + hour * 3600 + minute * 60 + second) * 1000000) + usec


def _parse_bound(raw, axis_dtype):
    if raw is None:
        return None
    if axis_dtype == "int":
        return _parse_int_str(raw)
    if axis_dtype == "datetime":
        from micromeerschaum import Timestamp
        body = raw[1:] if raw and raw[0] in ("+", "-") else raw
        if _digits(body):
            return Timestamp(_parse_int_str(raw))
        return Timestamp(_parse_iso_us(raw))
    raise ValueError("unsupported axis dtype " + str(axis_dtype))


def _format_doc(row):
    from micromeerschaum import Timestamp
    parts = []
    for k, v in row.items():
        rendered = ("Timestamp(%d)" % v.us) if isinstance(v, Timestamp) else repr(v)
        parts.append("%r: %s" % (k, rendered))
    return "{" + ", ".join(parts) + "}"


def _selection(words, copy, data=False):
    selectors, destination = {}, None
    limit, has_limit, begin, end = _DATA_DEFAULT_LIMIT, False, None, None
    explicit_target = copy and any(_ALIASES.get(w, w) == "-I" for w in words)
    i = 0
    while i < len(words):
        word = _ALIASES.get(words[i], words[i])
        i += 1
        if word in _FLAGS:
            values = selectors.setdefault(word, [])
            added = 0
            while i < len(words):
                nxt = words[i]
                if nxt.startswith("-"):
                    break
                if (added > 0 and copy and destination is None and
                        not explicit_target and _is_destination(nxt)):
                    break
                if not nxt and word != "-l":
                    raise ValueError("expected a non-empty value for " + word)
                values.append(nxt)
                added += 1
                i += 1
            if not added:
                raise ValueError("expected at least one value for " + word)
        elif data and word == "--limit":
            if has_limit:
                raise ValueError("duplicate --limit argument")
            if i == len(words) or not _digits(words[i]):
                raise ValueError("expected an integer between 0 and %d for --limit" % _DATA_MAX_LIMIT)
            limit = int(words[i])
            if not 0 <= limit <= _DATA_MAX_LIMIT:
                raise ValueError("expected an integer between 0 and %d for --limit" % _DATA_MAX_LIMIT)
            has_limit = True
            i += 1
        elif data and word == "--begin":
            if begin is not None:
                raise ValueError("duplicate --begin argument")
            if i == len(words) or not _is_bound_token(words[i]):
                raise ValueError("expected a value for --begin")
            begin = words[i]
            i += 1
        elif data and word == "--end":
            if end is not None:
                raise ValueError("duplicate --end argument")
            if i == len(words) or not _is_bound_token(words[i]):
                raise ValueError("expected a value for --end")
            end = words[i]
            i += 1
        elif copy and word == "-I":
            if destination is not None:
                raise ValueError("conflicting destination argument")
            if i == len(words) or words[i].startswith("-") or not _is_destination(words[i]):
                raise ValueError("expected a provisioned destination for -I")
            destination = words[i]
            i += 1
        elif copy and destination is None and _is_destination(word):
            destination = word
        else:
            raise ValueError("unexpected argument " + word)
    if copy and destination is None:
        raise ValueError("copy pipes requires a provisioned destination")
    if data:
        return selectors, limit, begin, end
    return selectors, destination


def _match_token(value, token, is_location):
    if is_location:
        if token in ("", "[None]"):
            return value is None
        if token == "None":
            return value is None or value == "None"
    return value == token


def _match_selector(value, tokens, is_location):
    has_positive = False
    found = False
    for token in tokens:
        if token.startswith("_"):
            if _match_token(value, token[1:], is_location):
                return False
        else:
            has_positive = True
            if _match_token(value, token, is_location):
                found = True
    return (not has_positive) or found


def _identity(pipe):
    return (pipe.connector_keys, pipe.metric_key, pipe.location_key or None, pipe.instance_keys)


class Exposure:
    def __init__(self, pipe, number, read, write):
        self.pipe = pipe
        self.registration = pipe._config()
        self.id = number
        self.read = read
        self.write = write
        self.identity = _identity(pipe)
        self.key = self.identity[:3]

    def live(self):
        try:
            return self.pipe._config() is self.registration and _identity(self.pipe) == self.identity
        except ValueError:
            return False

    def matches(self, selectors):
        for flag, tokens in selectors.items():
            field = self.identity[_FLAGS[flag]]
            if not _match_selector(field, tokens, flag == "-l"):
                return False
        return True

    def label(self):
        connector, metric, location, instance = self.identity
        parts = [repr(connector), repr(metric)]
        if location is not None:
            parts.append(repr(location))
        if instance != "journal:local":
            parts.append("instance=" + repr(instance))
        return "Pipe(" + ", ".join(parts) + ")"


def _validate_action_name(name):
    words = _words(name)
    if (not words or name != " ".join(words) or
            any(ord(c) > 126 or c in ("'", '"', "\\") for c in name)):
        raise ValueError("expected a canonical action name")
    return name


def _first_doc_line(text):
    if not isinstance(text, str):
        return None
    for line in text.replace("\r", "\n").split("\n"):
        stripped = line.strip()
        if stripped:
            return stripped
    return None


class ActionRegistry:
    """Live Pipes and actions for one application. All access is explicit."""
    def __init__(self):
        self._custom = {}
        self._help = {}
        self._aliases = {}
        self._pipes = {}
        self._next_id = 1

    def _alias_for(self, name):
        return _BUILTIN_ALIASES.get(name, self._aliases.get(name))

    def _canonical_name(self, name):
        if name in _BUILTINS or name in self._custom:
            return name
        for canon, alias in _BUILTIN_ALIASES.items():
            if alias == name:
                return canon
        for canon, alias in self._aliases.items():
            if alias == name:
                return canon
        return None

    def _taken(self, candidate):
        return self._canonical_name(candidate) is not None

    def _describe(self, name):
        text = name
        alias = self._alias_for(name)
        if alias:
            text += " (" + alias + ")"
        summary = self.help_for(name)
        if summary:
            text += "  " + summary
        return text

    def make_action(self, name=None, help=None, alias=None):
        if help is not None and not isinstance(help, str):
            raise ValueError("expected a help string")
        if alias is not None:
            _validate_action_name(alias)

        def register(action_name, function):
            if not callable(function):
                raise ValueError("expected an action function")
            if action_name is None:
                raw = getattr(function, "__name__", None)
                if not isinstance(raw, str) or raw == "<lambda>":
                    raise ValueError("expected a canonical action name")
                action_name = raw.replace("_", " ")
            _validate_action_name(action_name)
            if alias is not None and alias == action_name:
                raise ValueError("action alias must differ from name")
            if self._taken(action_name) or (alias is not None and self._taken(alias)):
                raise ValueError("action already registered")
            self._custom[action_name] = function
            if alias is not None:
                self._aliases[action_name] = alias
            summary = (_first_doc_line(help) if help is not None
                       else _first_doc_line(getattr(function, "__doc__", None)))
            if summary:
                self._help[action_name] = summary
            return function

        if callable(name):
            return register(None, name)
        if name is not None:
            _validate_action_name(name)
            if alias is not None and alias == name:
                raise ValueError("action alias must differ from name")
            if self._taken(name):
                raise ValueError("action already registered")
        if alias is not None and self._taken(alias):
            raise ValueError("action already registered")
        return lambda function: register(name, function)

    def help_for(self, name):
        """Return the stored one-line help text for a registered action, or None."""
        canon = self._canonical_name(name)
        if canon is None:
            return None
        if canon in _BUILTIN_HELP:
            return _BUILTIN_HELP[canon]
        return self._help.get(canon)

    def entries(self):
        """Snapshot live exposures in registration order."""
        for key in tuple(self._pipes):
            if not self._pipes[key].live():
                del self._pipes[key]
        return sorted(self._pipes.values(), key=lambda entry: entry.id)

    def expose(self, pipe, read=False, write=False):
        """Publish an open Pipe. Each device key identifies exactly one exposure."""
        from micromeerschaum import Pipe
        if not isinstance(pipe, Pipe) or type(read) is not bool or type(write) is not bool:
            raise ValueError("expected a Pipe and Boolean permissions")
        if (read or write) and not (pipe.instance_keys.startswith("journal:") or pipe.instance_keys == "sql:local"):
            raise ValueError("row access requires a local storage Pipe")
        self.entries()
        entry = Exposure(pipe, self._next_id, read, write)
        if entry.key in self._pipes:
            raise ValueError("Pipe key already exposed")
        self._pipes[entry.key] = entry
        self._next_id += 1
        return entry.id

    def unexpose(self, pipe):
        key = _identity(pipe)[:3]
        entry = self._pipes.get(key)
        if entry is not None and entry.pipe is pipe:
            del self._pipes[key]

    def names(self, allowed=None):
        names = list(_BUILTINS) + list(self._custom)
        return sorted(n for n in names if allowed is None or n in allowed)

    def complete(self, line="", allowed=None):
        words = _words(line)
        prefix = " ".join(words)
        if words and line[-1].isspace():
            prefix += " "
        return [name for name in self.names(allowed) if name.startswith(prefix)]

    def _help_action(self, name, args, allowed):
        visible = self.names(allowed)
        if name == "show actions":
            if args:
                return False, "Usage: show actions"
            return bool(visible), "\n".join(self._describe(n) for n in visible) if visible else "no matching actions"
        if not args:
            return bool(visible), "\n".join(self._describe(n) for n in visible) if visible else "no matching actions"
        query = " ".join(args)
        for canon in visible:
            if query == canon or query == self._alias_for(canon):
                return True, self._describe(canon)
        matched = []
        for canon in visible:
            alias = self._alias_for(canon)
            if canon.startswith(query) or (alias and alias.startswith(query)):
                matched.append(self._describe(canon))
        if matched:
            return True, "\n".join(matched)
        return False, "unknown action"

    def dispatch(self, line, allowed=None):
        """Run the longest matching action, passing its remaining words to the handler."""
        try:
            words = _words(line)
            name, i = None, 0
            for canon in self.names():
                alias = self._alias_for(canon)
                for candidate in (canon, alias) if alias else (canon,):
                    head = candidate.split(" ")
                    if len(head) > i and words[:len(head)] == head:
                        name, i = canon, len(head)
            if name is None:
                raise ValueError("unknown action")
            if allowed is not None and name not in allowed:
                raise ValueError("action unavailable")
            # Preserve the argument substring, including whitespace within quotes.
            at = 0
            for _ in range(i):
                while line[at] in " \t":
                    at += 1
                while at < len(line) and line[at] not in " \t":
                    at += 1
            args = _arguments(line[at:])
            if name in self._custom:
                result = self._custom[name](args)
                if not (isinstance(result, tuple) and len(result) == 2
                        and type(result[0]) is bool and isinstance(result[1], str)):
                    raise ValueError("action must return (bool, message)")
                return result
            if name in ("help", "show actions"):
                return self._help_action(name, args, allowed)
            if name == "show version":
                return self._show_version(args)
            if name == "show connectors":
                return self._show_connectors(args, allowed is not None)
            if name in ("show data", "show columns", "show rowcounts", "show progress"):
                return self._inspect_action(name, args, allowed is not None)
            return self._pipe_action(name, args, allowed is not None)
        except Exception as error:
            return False, str(error)

    def _show_version(self, args):
        if args:
            return False, "Usage: show version"
        import micromeerschaum
        return True, "MicroMeerschaum %s (MicroPython)" % micromeerschaum.__version__

    def _show_connectors(self, args, remote):
        import micromeerschaum
        filt = None
        if len(args) > 1:
            raise ValueError("unexpected argument " + args[1])
        if len(args) == 1:
            if args[0] not in _CONNECTOR_TYPES:
                raise ValueError("unexpected argument " + args[0])
            filt = args[0] + ":"
        found = set()
        for entry in self.entries():
            found.add(entry.pipe.instance_keys)
        if not remote:
            for reg in micromeerschaum._stores.values():
                found.add(reg.keys[3])
        for peer in micromeerschaum._peers:
            found.add(peer)
        items = sorted(c for c in found if filt is None or c.startswith(filt))
        return True, "\n".join(items) or "no matching connectors"

    def _inspect_action(self, name, args, remote):
        if name == "show data":
            selectors, limit, begin_raw, end_raw = _selection(args, False, data=True)
        else:
            selectors, _ = _selection(args, False)
            limit = begin_raw = end_raw = None
        selected = [e for e in self.entries() if e.matches(selectors)]
        if not selected:
            return True, "no matching Pipes"
        if remote and any(not e.read for e in selected):
            return False, "Pipe access denied"
        if name == "show data":
            return self._show_data(selected, limit, begin_raw, end_raw)
        if name == "show columns":
            return self._show_columns(selected)
        if name == "show rowcounts":
            lines = ["%s  %d" % (e.label(), e.pipe.get_rowcount()) for e in selected]
            return True, "\n".join(lines)
        lines = []
        for e in selected:
            state = e.pipe.progress()
            lines.append(
                "%s  prefix=%d high=%d confirmed=%d missed=%d" % (
                    e.label(), state["prefix"], state["high"], state["confirmed"], state["missed"]
                )
            )
        return True, "\n".join(lines)

    def _show_columns(self, selected):
        lines = []
        for entry in selected:
            lines.append(entry.label())
            if entry.pipe.instance_keys != "sql:local":
                lines.append("  (no fixed schema; journal pipe)")
                continue
            axis, cols, autotime = entry.pipe._config().store.definition()
            for col_name, dtype_name, is_key in cols:
                if col_name == axis:
                    role = " [axis] (autotime)" if autotime else " [axis]"
                elif is_key:
                    role = " [key]"
                else:
                    role = ""
                lines.append("  %s: %s%s" % (col_name, dtype_name, role))
        return True, "\n".join(lines)

    def _show_data(self, selected, limit, begin_raw, end_raw):
        parsed = []
        for entry in selected:
            pipe = entry.pipe
            if pipe.instance_keys == "sql:local":
                axis, cols, _ = pipe._config().store.definition()
                axis_dtype = None
                for col_name, dtype_name, _ in cols:
                    if col_name == axis:
                        axis_dtype = dtype_name
                        break
                parsed.append((entry, _parse_bound(begin_raw, axis_dtype), _parse_bound(end_raw, axis_dtype)))
            else:
                if begin_raw is not None or end_raw is not None:
                    raise ValueError("journal pipes do not support --begin or --end")
                parsed.append((entry, None, None))
        lines = []
        total = 0
        trunc_line = "  ... (truncated)"
        trunc_reserve = len(trunc_line) + 1
        truncated = False

        def add_line(line):
            nonlocal total
            size = len(line.encode()) + (1 if lines else 0)
            if total + size + trunc_reserve > _DATA_REPLY_LIMIT:
                return False
            lines.append(line)
            total += size
            return True

        for entry, begin_val, end_val in parsed:
            if not add_line(entry.label()):
                truncated = True
                break
            pipe = entry.pipe
            emitted = 0
            if pipe.instance_keys == "sql:local":
                try:
                    rows = pipe._config().store.docs(begin_val, end_val, None, limit, _DATA_REPLY_LIMIT * 2)
                except ValueError as err:
                    if "budget" in str(err):
                        truncated = True
                        break
                    raise
                for row in rows:
                    if not add_line("  " + _format_doc(row)):
                        truncated = True
                        break
                    emitted += 1
            else:
                try:
                    for row in pipe._retained_docs(limit, _DATA_REPLY_LIMIT + 64 * 16 + 32):
                        if not add_line("  " + _format_doc(row)):
                            truncated = True
                            break
                        emitted += 1
                except OSError as err:
                    if "too_small" in str(err):
                        truncated = True
                    else:
                        raise
            if truncated:
                break
            if not emitted:
                if not add_line("  (no rows)"):
                    truncated = True
                    break
        if truncated:
            lines.append(trunc_line)
        return True, "\n".join(lines)

    def _pipe_action(self, name, args, remote):
        selectors, destination = _selection(args, name == "copy pipes")
        selected = [e for e in self.entries() if e.matches(selectors)]
        if name == "show pipes":
            return True, "\n".join(e.label() for e in selected) or "no matching Pipes"
        if not selected:
            return False, "no matching Pipes"
        if name == "sync pipes":
            sources = [e for e in selected if e.pipe.connector_keys.startswith("plugin:")]
            if selectors and len(sources) != len(selected):
                return False, "push-only Pipe requires supplied rows"
            selected = sources
            if not selected:
                return True, "no configured source Pipes"
        if remote and any(not (e.write if name == "sync pipes" else e.read) for e in selected):
            return False, "Pipe access denied"
        results = []
        ok = True
        for entry in selected:
            result = entry.pipe.sync() if name == "sync pipes" else entry.pipe.copy_to(destination)
            ok = ok and result[0]
            results.append(("+ " if result[0] else "- ") + entry.label() + "  " + result[1])
        return ok, "\n".join(results)

    def shell(self, reader=None, writer=None, instance="journal:local",
              ansi=False, unicode=False, history_size=16):
        """Enter the device prompt. Ctrl-C cancels input. Exit or Ctrl-D at an empty line returns."""
        import sys
        reader = reader or sys.stdin
        writer = writer or sys.stdout

        def valid_instance(text):
            return (isinstance(text, str) and 1 <= len(text) <= 63 and
                    all(32 < ord(c) <= 126 and c not in ("'", '"', "\\") for c in text))

        if not valid_instance(instance):
            raise ValueError("invalid instance keys")
        if type(history_size) is not int or history_size < 0:
            raise ValueError("invalid history_size")

        default_instance = instance
        active_instance = instance
        filter_instance = False
        pipe_commands = (
            ("show", "pipes"), ("show", "pipe"),
            ("sync", "pipes"), ("sync", "pipe"),
            ("copy", "pipes"), ("copy", "pipe"),
            ("show", "data"), ("show", "datum"),
            ("show", "columns"), ("show", "column"),
            ("show", "rowcounts"), ("show", "rowcount"),
            ("show", "progress"),
        )

        def make_prompt():
            arrow = "\u27a4" if unicode else ">"
            if ansi:
                return "\x1b[32m[\x1b[0m \x1b[36m" + active_instance + "\x1b[0m \x1b[32m] " + arrow + "\x1b[0m "
            return "[ " + active_instance + " ] " + arrow + " "

        def status_marker(ok):
            if ansi:
                return "\x1b[32m+ \x1b[0m" if ok else "\x1b[31m- \x1b[0m"
            return "+ " if ok else "- "

        def format_reply(ok, message):
            if not message:
                return status_marker(ok) + "\n"
            lines = message.split("\n")
            if lines and lines[-1] == "":
                lines = lines[:-1]
            out = []
            for entry in lines:
                if entry.startswith("+ "):
                    out.append(status_marker(True) + entry[2:])
                elif entry.startswith("- "):
                    out.append(status_marker(False) + entry[2:])
                else:
                    out.append(status_marker(ok) + entry)
            return "\n".join(out) + "\n"

        def scope_line(raw):
            if not filter_instance:
                return raw
            try:
                words = _words(raw)
            except ValueError:
                return raw
            if len(words) < 2 or (words[0], words[1]) not in pipe_commands:
                return raw
            at = 0
            for _ in range(2):
                while at < len(raw) and raw[at] in " \t":
                    at += 1
                while at < len(raw) and raw[at] not in " \t":
                    at += 1
            try:
                args = _arguments(raw[at:])
            except ValueError:
                return raw
            for arg in args:
                if arg in ("-i", "--instance", "--mrsm-instance"):
                    return raw
            return raw + " -i " + active_instance

        history = []
        hist_pos = 0

        def push_history(entry):
            if history_size <= 0 or not entry:
                return
            if history and history[-1] == entry:
                return
            if len(history) >= history_size:
                del history[0]
            history.append(entry)

        def replace_line(current, replacement):
            if current:
                writer.write("\b \b" * len(current))
            if replacement:
                writer.write(replacement)
            return replacement

        try:
            import micromeerschaum as _mrsm
            version = getattr(_mrsm, "__version__", "0.2.1")
        except ImportError:
            version = "0.2.1"

        line, too_long, previous_cr = "", False, False
        esc_state, csi_has_param = 0, False
        writer.write("MicroMeerschaum " + version + "\n" + make_prompt())
        while True:
            try:
                char = reader.read(1)
            except KeyboardInterrupt:
                char = "\x03"
            if char == "\x03":
                line, too_long, previous_cr = "", False, False
                esc_state, csi_has_param, hist_pos = 0, False, 0
                writer.write("^C\n" + make_prompt())
                continue
            if not char or (char == "\x04" and not line and not too_long):
                break
            if char == "\n" and previous_cr:
                previous_cr = False
                continue
            previous_cr = char == "\r"
            if char in ("\n", "\r"):
                esc_state, csi_has_param, hist_pos = 0, False, 0
                writer.write("\n")
                stripped = line.strip()
                if too_long:
                    writer.write(format_reply(False, "action line too long"))
                elif stripped in ("exit", "quit"):
                    return
                elif stripped:
                    push_history(stripped)
                    if stripped == "clear":
                        writer.write("\x1b[2J\x1b[H")
                    elif stripped == "instance" or (stripped.startswith("instance") and stripped[8] in " \t"):
                        rest = stripped[8:].strip()
                        if not rest:
                            writer.write(format_reply(True, active_instance))
                        else:
                            parts = [w for w in rest.replace("\t", " ").split(" ") if w]
                            if len(parts) != 1:
                                writer.write(format_reply(False, "Usage: instance [KEYS|all|default|reset]"))
                            elif parts[0] in ("all", "default", "reset"):
                                active_instance = default_instance
                                filter_instance = False
                                writer.write(format_reply(True, "Active instance: " + active_instance))
                            elif not valid_instance(parts[0]):
                                writer.write(format_reply(False, "invalid instance keys"))
                            else:
                                active_instance = parts[0]
                                filter_instance = True
                                writer.write(format_reply(True, "Active instance: " + active_instance))
                    elif stripped.endswith("?"):
                        try:
                            matches = self.complete(stripped[:-1])
                            if matches:
                                writer.write(format_reply(True, "\n".join(matches)))
                            else:
                                writer.write(format_reply(False, "no matching actions"))
                        except ValueError as e:
                            writer.write(format_reply(False, str(e)))
                    else:
                        ok, message = self.dispatch(scope_line(line))
                        writer.write(format_reply(ok, message))
                line, too_long = "", False
                writer.write(make_prompt())
            elif too_long:
                continue
            elif esc_state == 1:
                if char == "[":
                    esc_state, csi_has_param = 2, False
                elif char == "O":
                    esc_state = 3
                else:
                    esc_state = 0
            elif esc_state == 2:
                code = ord(char)
                if 0x20 <= code <= 0x3F:
                    csi_has_param = True
                else:
                    esc_state = 0
                    if not csi_has_param and char == "A" and history:
                        if hist_pos < len(history):
                            hist_pos += 1
                            line = replace_line(line, history[-hist_pos])
                    elif not csi_has_param and char == "B" and history:
                        if hist_pos > 1:
                            hist_pos -= 1
                            line = replace_line(line, history[-hist_pos])
                        elif hist_pos == 1:
                            hist_pos = 0
                            line = replace_line(line, "")
            elif esc_state == 3:
                esc_state = 0
                if char == "A" and history:
                    if hist_pos < len(history):
                        hist_pos += 1
                        line = replace_line(line, history[-hist_pos])
                elif char == "B" and history:
                    if hist_pos > 1:
                        hist_pos -= 1
                        line = replace_line(line, history[-hist_pos])
                    elif hist_pos == 1:
                        hist_pos = 0
                        line = replace_line(line, "")
            elif char == "\x1b":
                esc_state = 1
            elif char in ("\b", "\x7f"):
                if line:
                    line = line[:-1]
                    hist_pos = 0
                    writer.write("\b \b")
            elif char == "\t":
                try:
                    matches = self.complete(line)
                except ValueError:
                    matches = []
                if len(matches) == 1:
                    line = matches[0] + " "
                    hist_pos = 0
                writer.write("\n" + "\n".join(matches) + "\n" + make_prompt() + line)
            elif ord(char) >= 32:
                line += char
                hist_pos = 0
                if len(line.encode()) > MAX_LINE:
                    too_long = True
                else:
                    writer.write(char)
        writer.write("\n")


default_actions = ActionRegistry()
make_action = default_actions.make_action
expose = default_actions.expose
unexpose = default_actions.unexpose
dispatch = default_actions.dispatch
complete = default_actions.complete
shell = default_actions.shell
