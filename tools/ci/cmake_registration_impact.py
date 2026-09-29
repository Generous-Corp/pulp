"""Which targets and tests an edit to a CMake test manifest reaches.

A registration edit changes a test's command, properties, build flags or the
variables they read, and none of that reaches the test through a source
dependency. Given a manifest's head text, the head-side lines a diff touched
and the text of the lines it removed, `impact()` names:

- the test or target a touched call belongs to (`add_test(NAME x)`,
  `target_*(x ...)`, `add_custom_command(TARGET x ...)`, ...);
- everything registered inside a block whose `if`/`foreach` header was touched;
- the owner of every call that reads a variable a changed or removed line
  writes, including a templated name such as `_prefix_${key}`, and everything
  registered inside a block whose condition reads one.

Only the manifest's own text is read, so a variable consumed from another file
is not followed.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

_ARG = re.compile(r'"((?:[^"\\]|\\.)*)"|([^\s()"]+)')
_CALL = re.compile(r"(?m)^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*\(")
_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_OPEN = {"if", "foreach", "while", "function", "macro", "block"}
_CLOSE = {"endif", "endforeach", "endwhile", "endfunction", "endmacro", "endblock"}
# Commands whose first argument is the target they configure.
_TARGET_FIRST = {"add_executable", "add_library", "add_dependencies",
                 "catch_discover_tests", "target_compile_definitions",
                 "target_compile_features", "target_compile_options",
                 "target_include_directories", "target_link_libraries",
                 "target_link_options", "target_sources", "target_precompile_headers"}
# Commands that write the variable named by their first argument.
_WRITES_FIRST = {"set", "unset", "option", "get_target_property", "get_property",
                 "get_filename_component", "get_test_property", "find_program",
                 "find_file", "find_path", "find_library"}
_OUT_KEYWORDS = {"OUTPUT_VARIABLE", "RESULT_VARIABLE", "ERROR_VARIABLE",
                 "RESULTS_VARIABLE"}
# A prefix shorter than this is too generic to follow.
_MIN_PREFIX = 6


@dataclass
class Call:
    command: str
    first: int
    last: int
    args: list[str]
    text: str
    block: "Block | None" = None


@dataclass
class Block:
    header: Call
    end: int = 0
    calls: list[Call] = field(default_factory=list)


@dataclass
class Impact:
    tests: set[str] = field(default_factory=set)
    targets: set[str] = field(default_factory=set)


def calls(text: str) -> list[Call]:
    """Every command call in `text` with its line span, comments skipped."""
    found: list[Call] = []
    for match in _CALL.finditer(text):
        if _in_comment(text, match.start()):
            continue
        depth, index = 1, match.end()
        while index < len(text) and depth:
            char = text[index]
            if char == "#":
                index = text.find("\n", index)
                if index < 0:
                    index = len(text)
                    break
                continue
            if char == '"':
                close = re.compile(r'(?<!\\)"').search(text, index + 1)
                index = close.end() if close else len(text)
                continue
            depth += {"(": 1, ")": -1}.get(char, 0)
            index += 1
        body = text[match.end():index - 1]
        found.append(Call(match.group(1).lower(), text.count("\n", 0, match.start()) + 1,
                          text.count("\n", 0, index) + 1,
                          [m.group(1) if m.group(1) is not None else m.group(2)
                           for m in _ARG.finditer(body)], body))
    return found


def _in_comment(text: str, offset: int) -> bool:
    line_start = text.rfind("\n", 0, offset) + 1
    return "#" in text[line_start:offset]


def _blocks(found: list[Call]) -> list[Block]:
    stack: list[Block] = []
    blocks: list[Block] = []
    for call in found:
        if stack:
            call.block = stack[-1]
            for open_block in stack:
                open_block.calls.append(call)
        if call.command in _OPEN:
            block = Block(call)
            stack.append(block)
            blocks.append(block)
        elif call.command in _CLOSE and stack:
            stack.pop().end = call.last
    return blocks


def _owner(call: Call, impact: Impact) -> None:
    args = call.args
    if not args:
        return
    if call.command == "add_test":
        if args[0] == "NAME" and len(args) > 1:
            impact.tests.add(args[1])
    elif call.command == "set_tests_properties":
        impact.tests.update(args[:args.index("PROPERTIES")] if "PROPERTIES" in args else [])
    elif call.command in _TARGET_FIRST:
        impact.targets.add(args[0])
    elif call.command == "set_target_properties":
        impact.targets.update(args[:args.index("PROPERTIES")] if "PROPERTIES" in args else [])
    elif call.command in {"add_custom_command", "set_property"} and len(args) > 1:
        scope = args[0]
        members = []
        for arg in args[1:]:
            if arg in {"APPEND", "APPEND_STRING", "PROPERTY", "PRE_BUILD", "PRE_LINK",
                       "POST_BUILD", "COMMAND"}:
                break
            members.append(arg)
        if scope == "TARGET":
            impact.targets.update(members)
        elif scope == "TEST":
            impact.tests.update(members)


def _written(call: Call) -> set[str]:
    """Variable names (or `prefix*` patterns) a call writes."""
    names: set[str] = set()
    args = call.args
    if not args:
        return names
    if call.command in _WRITES_FIRST:
        names.add(args[0])
    elif call.command in {"list", "string", "file", "math", "cmake_path",
                          "separate_arguments"} and len(args) > 1:
        names.add(args[1])
        names.add(args[-1])
    for keyword, value in zip(args, args[1:]):
        if keyword in _OUT_KEYWORDS:
            names.add(value)
    patterns: set[str] = set()
    for name in names:
        if _IDENT.match(name):
            patterns.add(name)
        elif "${" in name:
            prefix = name.split("${", 1)[0]
            if len(prefix) >= _MIN_PREFIX and _IDENT.match(prefix):
                patterns.add(prefix + "*")
    return patterns


def _reads(call: Call, pattern: str) -> bool:
    if pattern.endswith("*"):
        stem = re.escape(pattern[:-1]) + r"[A-Za-z0-9_${}]*"
    else:
        stem = re.escape(pattern)
    if re.search(r"\$\{" + stem + r"\}", call.text):
        return True
    # A bare name in a condition reads the variable too.
    return call.command in {"if", "elseif", "while"} and bool(
        re.search(r"(?<![A-Za-z0-9_${])" + stem + r"(?![A-Za-z0-9_])", call.text))


def _removed_writes(removed: list[str]) -> set[str]:
    patterns: set[str] = set()
    for call in calls("\n".join(removed)):
        patterns |= _written(call)
    return patterns


def impact(text: str, touched: set[int], removed: list[str] = ()) -> Impact:
    """The tests and targets an edit touching `touched` head lines reaches."""
    found = calls(text)
    blocks = _blocks(found)
    result = Impact()
    written: set[str] = _removed_writes(list(removed))
    reached_blocks: list[Block] = []
    for call in found:
        if not any(call.first <= line <= call.last for line in touched):
            continue
        _owner(call, result)
        written |= _written(call)
        if call.command in _OPEN or call.command in {"elseif", "else"}:
            block = next((b for b in blocks if b.header is call), call.block)
            if block is not None:
                reached_blocks.append(block)
    # A `foreach` loop variable is not followed: its body is the block.
    for call in found:
        if any(_reads(call, pattern) for pattern in written):
            _owner(call, result)
            if call.command in {"if", "elseif", "while", "foreach"}:
                block = next((b for b in blocks if b.header is call), call.block)
                if block is not None:
                    reached_blocks.append(block)
    for block in reached_blocks:
        for call in block.calls:
            _owner(call, result)
    return result
