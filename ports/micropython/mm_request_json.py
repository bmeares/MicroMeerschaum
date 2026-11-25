# SPDX-License-Identifier: Apache-2.0
"""Strict, depth-bounded JSON for requests that may mutate device state.

MicroPython's json.loads intentionally accepts missing separators, trailing
commas, and other non-JSON inputs. Its scalar decoder handles UTF-8 and numbers.
"""
import json
import math

_HEX = b"0123456789abcdefABCDEF"


def _text(raw):
    string = json.loads(raw.decode())
    if b"\\u" not in raw:
        return string
    # MicroPython 1.28 decodes each UTF-16 escape independently.
    chars = iter(string)
    result = []
    for char in chars:
        code = ord(char)
        if 0xd800 <= code <= 0xdbff:
            low = ord(next(chars, " "))
            if not 0xdc00 <= low <= 0xdfff:
                raise ValueError("invalid Unicode surrogate")
            char = chr(0x10000 + ((code - 0xd800) << 10) + low - 0xdc00)
        elif 0xdc00 <= code <= 0xdfff:
            raise ValueError("invalid Unicode surrogate")
        result.append(char)
    return "".join(result)


class _Parser:
    def __init__(self, data):
        self.data = data
        self.at = 0

    def peek(self):
        while self.at < len(self.data) and self.data[self.at] in b" \r\n\t":
            self.at += 1
        return self.data[self.at] if self.at < len(self.data) else -1

    def string(self):
        start = self.at
        self.at += 1
        while self.at < len(self.data):
            c = self.data[self.at]
            self.at += 1
            if c == 34:
                return _text(self.data[start:self.at])
            if c < 32:
                break
            if c == 92:
                escape = self.data[self.at]
                self.at += 1
                if escape == 117:  # 'u'
                    digits = self.data[self.at:self.at + 4]
                    if len(digits) != 4 or any(d not in _HEX for d in digits):
                        break
                    self.at += 4
                elif escape not in b'"\\/bfnrt':
                    break
        raise ValueError("invalid JSON string")

    def digits(self):
        start = self.at
        while self.at < len(self.data) and 48 <= self.data[self.at] <= 57:
            self.at += 1
        if self.at == start:
            raise ValueError("expected digits")

    def number(self):
        start = self.at
        end = len(self.data)
        if self.at < end and self.data[self.at] == 45:
            self.at += 1
        if self.at < end and self.data[self.at] == 48:
            self.at += 1
        else:
            self.digits()
        if self.at < end and self.data[self.at] == 46:
            self.at += 1
            self.digits()
        if self.at < end and self.data[self.at] in (69, 101):
            self.at += 1
            if self.at < end and self.data[self.at] in (43, 45):
                self.at += 1
            self.digits()
        value = json.loads(self.data[start:self.at])
        if type(value) is float and not math.isfinite(value):
            raise ValueError("number exceeds device range")
        return value

    def value(self, depth):
        if depth > 6:
            raise ValueError("request too deeply nested")
        c = self.peek()
        if c == 34:
            return self.string()
        if c in (123, 91):
            obj = c == 123
            end = 125 if obj else 93
            self.at += 1
            result = {} if obj else []
            if self.peek() == end:
                self.at += 1
                return result
            while True:
                if obj:
                    if self.peek() != 34:
                        raise ValueError("expected key")
                    key = self.string()
                    if key in result or self.peek() != 58:
                        raise ValueError("duplicate or invalid key")
                    self.at += 1
                    result[key] = self.value(depth + 1)
                else:
                    result.append(self.value(depth + 1))
                sep = self.peek()
                self.at += 1
                if sep == end:
                    return result
                if sep != 44:
                    raise ValueError("invalid JSON separator")
        for token, val in ((b"true", True), (b"false", False), (b"null", None)):
            if self.data[self.at:self.at + len(token)] == token:
                self.at += len(token)
                return val
        return self.number()


def loads(data):
    parser = _Parser(data)
    value = parser.value(0)
    if parser.peek() != -1:
        raise ValueError("trailing JSON")
    return value
