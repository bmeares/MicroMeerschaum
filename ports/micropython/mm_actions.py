# SPDX-License-Identifier: Apache-2.0
"""Named device actions and an interactive prompt for the MicroPython REPL."""

_PROMPT = "[ journal:local ] > "
MAX_LINE = 4224
_FLAGS = {"-c": 0, "-m": 1, "-l": 2, "-i": 3}
_ALIASES = {"--connector-keys": "-c", "--metric-keys": "-m", "--location-keys": "-l",
            "--mrsm-instance": "-i", "--instance": "-i", "--target-instance": "-I"}
_BUILTINS = ("help", "show actions", "show pipes", "sync pipes", "copy pipes")
_BUILTIN_ALIASES = {
    "show actions": "show action",
    "show pipes": "show pipe",
    "sync pipes": "sync pipe",
    "copy pipes": "copy pipe",
}
_BUILTIN_HELP = {
    "help": "help [ACTION]",
    "show actions": "List available actions.",
    "show pipes": "List matching configured Pipes.",
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


def _selection(words, copy):
    selectors, destination = {}, None
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
            return self._pipe_action(name, args, allowed is not None)
        except Exception as error:
            return False, str(error)

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

    def shell(self, reader=None, writer=None):
        """Enter the device prompt. Ctrl-C cancels input. Exit or Ctrl-D at an empty line returns."""
        import sys
        reader = reader or sys.stdin
        writer = writer or sys.stdout
        line, too_long, previous_cr = "", False, False
        writer.write(_PROMPT)
        while True:
            try:
                char = reader.read(1)
            except KeyboardInterrupt:
                char = "\x03"
            if char == "\x03":
                line, too_long, previous_cr = "", False, False
                writer.write("^C\n" + _PROMPT)
                continue
            if not char or (char == "\x04" and not line and not too_long):
                break
            if char == "\n" and previous_cr:
                previous_cr = False
                continue
            previous_cr = char == "\r"
            if char in ("\n", "\r"):
                writer.write("\n")
                if too_long:
                    writer.write("action line too long\n")
                elif line.strip() in ("exit", "quit"):
                    return
                elif line.strip():
                    if line.endswith("?"):
                        try:
                            message = "\n".join(self.complete(line[:-1]))
                        except ValueError as e:
                            message = str(e)
                    else:
                        _, message = self.dispatch(line)
                    writer.write(message + "\n")
                line, too_long = "", False
                writer.write(_PROMPT)
            elif too_long:
                continue
            elif char in ("\b", "\x7f"):
                if line:
                    line = line[:-1]
                    writer.write("\b \b")
            elif char == "\t":
                try:
                    matches = self.complete(line)
                except ValueError:
                    matches = []
                if len(matches) == 1:
                    line = matches[0] + " "
                writer.write("\n" + "\n".join(matches) + "\n" + _PROMPT + line)
            elif ord(char) >= 32:
                line += char
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
