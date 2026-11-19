#!/usr/bin/env python3
"""Tokenize CMake source into the command invocations CMake would run.

A text search over CMake cannot tell a real command from the same characters
inside a comment, a quoted argument or a bracket argument. This module
implements the CMake language grammar (cmake-language(7)) closely enough to
separate them: only command invocations come out, and every argument records
whether it was unquoted, quoted or a bracket argument.

`tokenize` returns every invocation in source order. `executable_commands`
additionally drops the ones CMake cannot run where they stand:

- commands inside a function() or macro() body, which only run if called;
- commands in an if/elseif/else branch that a constant condition rules out
  (``if(FALSE)``, ``if(NOT ON)``, an else after ``if(TRUE)``), and in a
  ``while()`` whose condition is constant false;
- commands after a ``return()`` that is not provably conditional. A return()
  ends the file unless some enclosing if/elseif/else branch depends on a
  non-constant condition; one inside ``if(TRUE)``, the else of ``if(FALSE)``,
  a foreach(), a while() or a block() is taken to end the file. That errs
  toward dropping commands, so an audit relying on them fails rather than
  passes.

What it still does not model: conditions on variables, commands generated at
configure time (``cmake_language(EVAL)``, ``include()`` of a generated file),
and calls to a function or macro defined elsewhere. Callers that need those
must assert on them separately.

Malformed input (an unterminated quoted argument, bracket argument, bracket
comment or argument list, or an unbalanced block) raises CMakeSyntaxError, so
an audit built on this fails closed instead of guessing.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

UNQUOTED = "unquoted"
QUOTED = "quoted"
BRACKET = "bracket"

_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_BRACKET_OPEN = re.compile(r"\[(=*)\[")
_SPACE = " \t"

_OPENERS = {"if": "endif", "foreach": "endforeach", "while": "endwhile",
            "function": "endfunction", "macro": "endmacro", "block": "endblock"}
_CLOSERS = {closer: opener for opener, closer in _OPENERS.items()}
_FALSE_CONSTANTS = {"0", "off", "no", "false", "n", "ignore", "notfound", ""}
_TRUE_CONSTANTS = {"1", "on", "yes", "true", "y"}


class CMakeSyntaxError(ValueError):
    """The source is not valid CMake as far as this tokenizer can tell."""


@dataclass(frozen=True)
class Argument:
    value: str
    kind: str


@dataclass(frozen=True)
class Command:
    """One command invocation. `name` is lower-cased: CMake command names are
    case-insensitive. `blocks` is the stack of enclosing (block id, branch)
    pairs, outermost first, where branch counts the if/elseif/else arms from 0;
    `opens` is the id of the block this command opens. Both are filled in only
    by `executable_commands`."""

    name: str
    arguments: tuple[Argument, ...]
    line: int
    blocks: tuple[tuple[int, int], ...] = field(default=())
    opens: int | None = None

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
        if nxt in "trn;":
            # Encoded escapes keep their spelling; CMake decodes them later.
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


def constant_condition(arguments: tuple[Argument, ...]) -> bool | None:
    """True or False when an if/while condition is a constant, else None."""
    values = [argument.value for argument in arguments]
    negate = False
    while values and values[0] == "NOT" and arguments[len(arguments) - len(values)].kind == UNQUOTED:
        negate = not negate
        values = values[1:]
    if len(values) != 1:
        return None
    word = values[0].lower()
    if "${" in word or "$env{" in word or "$cache{" in word:
        return None
    if word in _FALSE_CONSTANTS or word.endswith("-notfound"):
        result = False
    elif re.fullmatch(r"[+-]?(\d+\.?\d*|\.\d+)", word):
        result = float(word) != 0
    elif word in _TRUE_CONSTANTS:
        result = True
    else:
        return None
    return result != negate


@dataclass
class _Block:
    ident: int
    kind: str
    command: Command
    dead: bool  # this branch cannot run
    decided: bool = False  # an earlier if/elseif branch is constant true
    branch: int = 0
    certain: bool = False  # this if/elseif/else branch is always taken
    unknown: bool = False  # some branch so far has a non-constant condition


def executable_commands(text: str) -> list[Command]:
    """The invocations CMake can run where they stand, each tagged with its enclosing blocks.

    Block openers (if, foreach, while, block) are returned when live, with
    `opens` set, so a caller can ask which block and branch a command sits in.
    else/elseif/end* commands are not returned.
    """
    stack: list[_Block] = []
    live: list[Command] = []
    returned = False
    next_id = 0
    for command in tokenize(text):
        name = command.name
        outer_dead = returned or any(block.dead for block in stack)
        if name in ("elseif", "else"):
            if not stack or stack[-1].kind != "if":
                raise CMakeSyntaxError(f"line {command.line}: {name}() outside if()")
            block = stack[-1]
            block.branch += 1
            if name == "elseif":
                condition = constant_condition(command.arguments)
                block.dead = block.decided or condition is False
                block.certain = not block.decided and not block.unknown and condition is True
                block.decided = block.decided or condition is True
                block.unknown = block.unknown or condition is None
            else:
                block.dead = block.decided
                block.certain = not block.decided and not block.unknown
                block.decided = True
            continue
        if name in _CLOSERS:
            if not stack or stack[-1].kind != _CLOSERS[name]:
                raise CMakeSyntaxError(f"line {command.line}: {name}() without matching "
                                       f"{_CLOSERS[name]}()")
            stack.pop()
            continue
        tagged = Command(name, command.arguments, command.line,
                         tuple((block.ident, block.branch) for block in stack),
                         next_id if name in _OPENERS else None)
        if not outer_dead and not any(block.kind in ("function", "macro") for block in stack):
            live.append(tagged)
            if name == "return" and not any(block.kind == "if" and not block.certain
                                            for block in stack):
                returned = True
        if name in _OPENERS:
            dead = decided = unknown = False
            if name in ("if", "while"):
                condition = constant_condition(command.arguments)
                dead = condition is False
                decided = condition is True
                unknown = condition is None
            stack.append(_Block(next_id, name, tagged, dead, decided,
                                certain=decided, unknown=unknown))
            next_id += 1
    if stack:
        raise CMakeSyntaxError(f"line {stack[-1].command.line}: {stack[-1].kind}() is never closed")
    return live
