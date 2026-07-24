#!/usr/bin/env python3
"""Fail when WAM/WebCLAP omit a production timebase/timeline/playback TU."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import pathlib
import re
import sys


SUBSYSTEMS = ("timebase", "timeline", "playback")
PORTABLE_DEPENDENCIES = (
    "core/audio/src/rolling_audio_capture_buffer.cpp",
    "core/runtime/src/sha256.cpp",
)


@dataclass(frozen=True)
class Lane:
    cmake_file: str
    source_variable: str
    root_variable: str
    timeline_variable: str
    playback_variable: str
    target: str


LANES = {
    "WAM": Lane("tools/cmake/PulpWam.cmake", "_PULP_WAM_CORE_SOURCES",
                "_PULP_WAM_ROOT", "_PULP_WAM_TIMELINE_SOURCES",
                "_PULP_WAM_PLAYBACK_SOURCES", "pulp-wam-dsp"),
    "WebCLAP": Lane("tools/cmake/PulpWclap.cmake", "_PULP_WCLAP_CORE_SOURCES",
                    "_PULP_WCLAP_ROOT", "_PULP_WCLAP_TIMELINE_SOURCES",
                    "_PULP_WCLAP_PLAYBACK_SOURCES",
                    "pulp-wclap-dsp"),
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
    timeline_manifest = root / "core/timeline/PulpTimelineSources.cmake"
    timeline_sources: set[str] | None = None
    if timeline_manifest.exists():
        try:
            manifest_commands = cmake_commands(
                timeline_manifest.read_text(encoding="utf-8"))
            manifest_sets = arguments_for(
                manifest_commands, "set", "_PULP_TIMELINE_PORTABLE_SOURCE_FILES")
            if len(manifest_sets) == 1:
                timeline_sources = {
                    f"core/timeline/src/{source}"
                    for source in manifest_sets[0]
                    if re.fullmatch(r"[^\s/)]+\.cpp", source)
                }
        except (OSError, ValueError):
            timeline_sources = None

    playback_manifest = root / "core/playback/PulpPlaybackSources.cmake"
    playback_sources: set[str] | None = None
    if playback_manifest.exists():
        try:
            manifest_commands = cmake_commands(
                playback_manifest.read_text(encoding="utf-8"))
            manifest_sets = arguments_for(
                manifest_commands, "set", "_PULP_PLAYBACK_SOURCE_FILES")
            if len(manifest_sets) == 1:
                playback_sources = {
                    f"core/playback/src/{source}"
                    for source in manifest_sets[0]
                    if re.fullmatch(r"[^\s/)]+\.cpp", source)
                }
        except (OSError, ValueError):
            playback_sources = None

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
        all_listed = {
            source[len(source_prefix):]
            for source in source_sets[0]
            if source.startswith(source_prefix) and source.endswith(".cpp")
        }
        timeline_reference = "${" + lane.timeline_variable + "}"
        if timeline_reference not in source_sets[0]:
            failures.append(
                f"{lane_name}: shared timeline source manifest is not consumed")
        else:
            resolutions = arguments_for(
                commands, "pulp_resolve_timeline_sources",
                "${" + lane.root_variable + "}")
            resolved = any(arguments and arguments[-1] == lane.timeline_variable
                           for arguments in resolutions)
            if timeline_sources is None or not resolved:
                failures.append(
                    f"{lane_name}: invalid shared timeline source manifest wiring")
            else:
                all_listed.update(timeline_sources)
        playback_reference = "${" + lane.playback_variable + "}"
        if playback_reference in source_sets[0]:
            resolutions = arguments_for(
                commands, "pulp_resolve_playback_sources",
                "${" + lane.root_variable + "}")
            resolved = any(arguments and arguments[-1] == lane.playback_variable
                           for arguments in resolutions)
            if playback_sources is None or not resolved:
                failures.append(
                    f"{lane_name}: invalid shared playback source manifest wiring")
            else:
                all_listed.update(playback_sources)
        listed = {
            source
            for source in all_listed
            if re.fullmatch(
                r"core/(?:timebase|timeline|playback)/src/[^\s)]+\.cpp", source)
        }
        missing = sorted(expected - listed)
        if missing:
            failures.append(f"{lane_name}: missing " + ", ".join(missing))
        missing_dependencies = sorted(set(PORTABLE_DEPENDENCIES) - all_listed)
        if missing_dependencies:
            failures.append(
                f"{lane_name}: missing portable dependency " +
                ", ".join(missing_dependencies))

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
