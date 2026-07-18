#!/usr/bin/env python3
"""Fail when WAM/WebCLAP omit a production timebase/timeline/playback TU."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import pathlib
import re
import sys


SUBSYSTEMS = ("timebase", "timeline", "playback")


@dataclass(frozen=True)
class Lane:
    cmake_file: str
    source_variable: str
    root_variable: str
    target: str


LANES = {
    "WAM": Lane("tools/cmake/PulpWam.cmake", "_PULP_WAM_CORE_SOURCES",
                "_PULP_WAM_ROOT", "pulp-wam-dsp"),
    "WebCLAP": Lane("tools/cmake/PulpWclap.cmake", "_PULP_WCLAP_CORE_SOURCES",
                    "_PULP_WCLAP_ROOT", "pulp-wclap-dsp"),
}


def strip_cmake_comments(text: str) -> str:
    """Remove CMake line/block comments while preserving quoted # characters."""
    output: list[str] = []
    index = 0
    quoted = False
    escaped = False
    block_start = re.compile(r"#\[(=*)\[")
    while index < len(text):
        character = text[index]
        if escaped:
            output.append(character)
            escaped = False
            index += 1
            continue
        if character == "\\" and quoted:
            output.append(character)
            escaped = True
            index += 1
            continue
        if character == '"':
            output.append(character)
            quoted = not quoted
            index += 1
            continue
        if character != "#" or quoted:
            output.append(character)
            index += 1
            continue

        block = block_start.match(text, index)
        if block:
            terminator = "]" + block.group(1) + "]"
            end = text.find(terminator, block.end())
            comment_end = len(text) if end < 0 else end + len(terminator)
            output.extend("\n" for character in text[index:comment_end] if character == "\n")
            index = comment_end
            continue

        newline = text.find("\n", index)
        if newline < 0:
            break
        output.append("\n")
        index = newline + 1
    return "".join(output)


def cmake_commands(text: str) -> list[tuple[str, str]]:
    """Return CMake command names and balanced argument bodies, excluding comments."""
    clean = strip_cmake_comments(text)
    commands: list[tuple[str, str]] = []
    cursor = 0
    command_start = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
    while match := command_start.search(clean, cursor):
        depth = 1
        index = match.end()
        quoted = False
        escaped = False
        while index < len(clean) and depth:
            character = clean[index]
            if escaped:
                escaped = False
            elif character == "\\" and quoted:
                escaped = True
            elif character == '"':
                quoted = not quoted
            elif not quoted:
                if character == "(":
                    depth += 1
                elif character == ")":
                    depth -= 1
            index += 1
        if depth:
            raise ValueError(f"unterminated {match.group(1)}(...)")
        commands.append((match.group(1).lower(), clean[match.end():index - 1]))
        cursor = index
    return commands


def tokens(arguments: str) -> list[str]:
    return [token.strip('"') for token in re.findall(r'"(?:\\.|[^"\\])*"|[^\s]+', arguments)]


def arguments_for(commands: list[tuple[str, str]], command: str,
                  target: str) -> list[list[str]]:
    matches: list[list[str]] = []
    for name, body in commands:
        arguments = tokens(body)
        if name == command and arguments and arguments[0] == target:
            matches.append(arguments[1:])
    return matches


def check_root(root: pathlib.Path) -> tuple[int, list[str]]:
    root = root.resolve()
    expected = {
        path.relative_to(root).as_posix()
        for subsystem in SUBSYSTEMS
        for path in (root / "core" / subsystem / "src").rglob("*.cpp")
    }

    failures: list[str] = []
    for lane_name, lane in LANES.items():
        cmake = root / lane.cmake_file
        try:
            commands = cmake_commands(cmake.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            failures.append(f"{lane_name}: {error}")
            continue

        source_sets = arguments_for(commands, "set", lane.source_variable)
        if len(source_sets) != 1:
            failures.append(
                f"{lane_name}: expected exactly one set({lane.source_variable} ...), "
                f"found {len(source_sets)}")
            continue
        source_prefix = "${" + lane.root_variable + "}/"
        listed = {
            source[len(source_prefix):]
            for source in source_sets[0]
            if source.startswith(source_prefix) and source.endswith(".cpp") and
            re.fullmatch(r"core/(?:timebase|timeline|playback)/src/[^\s)]+\.cpp",
                         source[len(source_prefix):])
        }
        missing = sorted(expected - listed)
        if missing:
            failures.append(f"{lane_name}: missing " + ", ".join(missing))

        libraries = arguments_for(commands, "add_library", lane.target)
        source_reference = "${" + lane.source_variable + "}"
        if not any(arguments and arguments[0] == "OBJECT" and
                   source_reference in arguments[1:] for arguments in libraries):
            failures.append(
                f"{lane_name}: {lane.target} must be an OBJECT library consuming "
                f"{source_reference}")

        features = {token for arguments in arguments_for(
            commands, "target_compile_features", lane.target) for token in arguments}
        if "cxx_std_20" not in features:
            failures.append(f"{lane_name}: {lane.target} missing compile feature cxx_std_20")

        definitions = {token for arguments in arguments_for(
            commands, "target_compile_definitions", lane.target) for token in arguments}
        if "PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1" not in definitions:
            failures.append(
                f"{lane_name}: {lane.target} missing compile definition "
                "PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1")

        options = {token for arguments in arguments_for(
            commands, "target_compile_options", lane.target) for token in arguments}
        missing_options = sorted({"-fno-exceptions", "-fno-rtti"} - options)
        if missing_options:
            failures.append(
                f"{lane_name}: {lane.target} missing compile options " +
                ", ".join(missing_options))

    return len(expected), failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    expected_count, failures = check_root(args.root)

    if failures:
        for failure in failures:
            print(f"web-timeline-source-closure: {failure}", file=sys.stderr)
        return 1
    print(f"web-timeline-source-closure: OK ({expected_count} production TUs in both lanes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
