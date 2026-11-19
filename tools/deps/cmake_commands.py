#!/usr/bin/env python3
"""Tokenize CMake source into its command invocations.

A text search over CMake cannot tell a real command from the same characters
inside a comment, a quoted argument or a bracket argument. This module
implements the CMake language grammar (cmake-language(7)) closely enough to
separate them: only command invocations come out, and every argument records
whether it was unquoted, quoted or a bracket argument.

`tokenize` returns every invocation in source order, with no notion of
which ones would run: it answers "does any command write X", not "what does
this file do". Evaluating CMake is left to CMake (see
tools/deps/gpu_runtime_driver.cmake).

Malformed input (an unterminated quoted argument, bracket argument, bracket
comment or argument list) raises CMakeSyntaxError, so an audit built on this
fails closed instead of guessing.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

UNQUOTED = "unquoted"
QUOTED = "quoted"
BRACKET = "bracket"

_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_BRACKET_OPEN = re.compile(r"\[(=*)\[")
_SPACE = " \t"


class CMakeSyntaxError(ValueError):
    """The source is not valid CMake as far as this tokenizer can tell."""


@dataclass(frozen=True)
class Argument:
    value: str
    kind: str


@dataclass(frozen=True)
class Command:
    """One command invocation. `name` is lower-cased: CMake command names are
    case-insensitive."""

    name: str
    arguments: tuple[Argument, ...]
    line: int

    def values(self) -> list[str]:
        return [argument.value for argument in self.arguments]


class _Reader:
    def __init__(self, text: str) -> None:
        self.text = text
        self.index = 0
        self.length = len(text)

    def line(self, index: int | None = None) -> int:
        return self.text.count("\n", 0, self.index if index is None else index) + 1

    def peek(self, offset: int = 0) -> str:
        position = self.index + offset
        return self.text[position] if position < self.length else ""

    def fail(self, message: str, index: int | None = None) -> CMakeSyntaxError:
        return CMakeSyntaxError(f"line {self.line(index)}: {message}")

    def bracket_body(self, what: str) -> str:
        """At a ``[=*[`` opener, consume through the matching close and return the content."""
        match = _BRACKET_OPEN.match(self.text, self.index)
        assert match is not None
        start = self.index
        closing = "]" + match.group(1) + "]"
        body_start = match.end()
        end = self.text.find(closing, body_start)
        if end < 0:
            raise self.fail(f"unterminated {what}", start)
        self.index = end + len(closing)
        body = self.text[body_start:end]
        # A newline straight after the opener is not part of the content.
        if body.startswith("\r\n"):
            body = body[2:]
        elif body.startswith("\n"):
            body = body[1:]
        return body

    def skip_comment(self) -> None:
        """At a ``#``: consume a bracket comment, or a line comment up to (not including) the newline."""
        if _BRACKET_OPEN.match(self.text, self.index + 1):
            self.index += 1
            self.bracket_body("bracket comment")
            return
        end = self.text.find("\n", self.index)
        self.index = self.length if end < 0 else end

    def escape(self) -> str:
        """At a backslash: consume one escape sequence and return its value."""
        start = self.index
        nxt = self.peek(1)
        if nxt == "":
            raise self.fail("backslash at end of file", start)
        self.index += 2
        if nxt in "trn;$":
            # Encoded escapes keep their spelling; CMake decodes them later.
            # \$ stays escaped so a literal "\${X}" never reads as the
            # expanding reference "${X}".
            return "\\" + nxt
        if nxt.isalnum():
            raise self.fail(f"invalid escape sequence \\{nxt}", start)
        return nxt

    def quoted(self) -> str:
        start = self.index
        self.index += 1
        out: list[str] = []
        while True:
            char = self.peek()
            if char == "":
                raise self.fail("unterminated quoted argument", start)
            if char == '"':
                self.index += 1
                return "".join(out)
            if char == "\\":
                if self.peek(1) == "\n":
                    self.index += 2  # line continuation
                    continue
                if self.peek(1) == "\r" and self.peek(2) == "\n":
                    self.index += 3
                    continue
                out.append(self.escape())
                continue
            out.append(char)
            self.index += 1

    def unquoted(self) -> str:
        out: list[str] = []
        while True:
            char = self.peek()
            if char == "" or char in " \t\r\n()#":
                break
            if char == "\\":
                out.append(self.escape())
                continue
            if char == '"':
                # Legacy unquoted form: a quoted run embedded in an unquoted
                # argument (-Da="b c") stays part of that argument.
                out.append('"' + self.quoted() + '"')
                continue
            out.append(char)
            self.index += 1
        return "".join(out)

    def arguments(self, open_index: int) -> list[Argument]:
        """After the command's ``(``: consume through its matching ``)``."""
        args: list[Argument] = []
        depth = 1
        while True:
            char = self.peek()
            if char == "":
                raise self.fail("unterminated command argument list", open_index)
            if char in " \t\r\n":
                self.index += 1
            elif char == "#":
                self.skip_comment()
            elif char == "(":
                depth += 1
                args.append(Argument("(", UNQUOTED))
                self.index += 1
            elif char == ")":
                depth -= 1
                self.index += 1
                if depth == 0:
                    return args
                args.append(Argument(")", UNQUOTED))
            elif char == '"':
                args.append(Argument(self.quoted(), QUOTED))
            elif char == "[" and _BRACKET_OPEN.match(self.text, self.index):
                args.append(Argument(self.bracket_body("bracket argument"), BRACKET))
            else:
                value = self.unquoted()
                if value == "":
                    raise self.fail(f"unexpected {char!r} in argument list")
                args.append(Argument(value, UNQUOTED))


def tokenize(text: str) -> list[Command]:
    """Every command invocation in `text`, in source order, without execution context."""
    reader = _Reader(text)
    commands: list[Command] = []
    while reader.index < reader.length:
        char = reader.peek()
        if char in " \t\r\n":
            reader.index += 1
            continue
        if char == "#":
            reader.skip_comment()
            continue
        match = _IDENTIFIER.match(text, reader.index)
        if not match:
            raise reader.fail(f"expected a command, found {char!r}")
        start = reader.index
        reader.index = match.end()
        while reader.peek() and reader.peek() in _SPACE:
            reader.index += 1
        if reader.peek() != "(":
            raise reader.fail(f"command {match.group(0)!r} is not followed by '('", start)
        open_index = reader.index
        reader.index += 1
        args = reader.arguments(open_index)
        commands.append(Command(match.group(0).lower(), tuple(args), reader.line(start)))
        # A command must end its line (comments and spaces allowed before the newline).
        while reader.peek() and reader.peek() in _SPACE:
            reader.index += 1
        if reader.peek() == "#":
            reader.skip_comment()
        if reader.peek() not in ("", "\n", "\r"):
            raise reader.fail("a command must be followed by a newline")
    return commands
