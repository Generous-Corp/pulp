#!/usr/bin/env python3
"""Enforce the timeline engine modules' dependency floors.

Upper adapter/host/UI/render layers may consume the engine, but engine modules
may include and link only their explicitly declared lower-layer closure.
"""

from __future__ import annotations

import argparse
import re
import shutil
import tempfile
from pathlib import Path


MODULE_FLOORS = {
    "timebase": {"timebase", "platform", "runtime"},
    "timeline": {"timeline", "timebase", "platform", "runtime"},
    "playback": {
        "playback",
        "timeline",
        "timebase",
        "audio",
        "midi",
        "platform",
        "runtime",
    },
}
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]pulp/([^/]+)/", re.MULTILINE)
LINK_RE = re.compile(r"\bpulp(?:::|-)([a-zA-Z0-9_-]+)\b")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".mm"}
LINK_SCOPE_KEYWORDS = {"PUBLIC", "PRIVATE", "INTERFACE"}
COMMAND_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
ARGUMENT_RE = re.compile(r'"(?:\\.|[^"\\])*"|[^\s]+')


def strip_cmake_comments(text: str) -> str:
    """Drop CMake line and block comments, preserving quoted '#' characters."""
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
            output.extend("\n" for symbol in text[index:comment_end] if symbol == "\n")
            index = comment_end
            continue
        newline = text.find("\n", index)
        if newline < 0:
            break
        output.append("\n")
        index = newline + 1
    return "".join(output)


def link_dependencies(cmake_text: str) -> list[str]:
    """Return module tokens linked as dependencies via target_link_libraries.

    Only the libraries a target depends on count as links. The target being
    configured (the first argument) and target-defining commands such as
    add_executable/add_library are not dependencies, so a helper target whose
    own name shares the module prefix (for example pulp-timeline-schema-emit) is
    not misread as an outside-floor link.
    """
    text = strip_cmake_comments(cmake_text)
    dependencies: list[str] = []
    cursor = 0
    while match := COMMAND_RE.search(text, cursor):
        depth = 1
        index = match.end()
        quoted = False
        escaped = False
        while index < len(text) and depth:
            character = text[index]
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
        if match.group(1).lower() == "target_link_libraries":
            arguments = [token.strip('"')
                         for token in ARGUMENT_RE.findall(text[match.end():index - 1])]
            for token in arguments[1:]:
                if token in LINK_SCOPE_KEYWORDS:
                    continue
                found = LINK_RE.search(token)
                if found:
                    dependencies.append(found.group(1))
        cursor = index
    return dependencies


def verify(repo_root: Path) -> list[str]:
    errors: list[str] = []
    for module, allowed in MODULE_FLOORS.items():
        module_root = repo_root / "core" / module
        if not module_root.is_dir():
            errors.append(f"missing required engine module: {module_root}")
            continue

        for path in sorted(module_root.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            for match in INCLUDE_RE.finditer(path.read_text(errors="replace")):
                if match.group(1) not in allowed:
                    errors.append(
                        f"{path.relative_to(repo_root)}: {module} outside-floor "
                        f"pulp/{match.group(1)} include"
                    )

        cmake = module_root / "CMakeLists.txt"
        if not cmake.is_file():
            errors.append(f"missing {module} build file: {cmake}")
            continue
        for dependency in link_dependencies(cmake.read_text(errors="replace")):
            if dependency not in allowed:
                errors.append(
                    f"{cmake.relative_to(repo_root)}: {module} outside-floor "
                    f"pulp::{dependency} link"
                )
    return errors


def run_selftest() -> int:
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        fixtures: dict[str, tuple[Path, Path]] = {}
        for module, allowed in MODULE_FLOORS.items():
            module_root = root / "core" / module
            (module_root / "src").mkdir(parents=True)
            source = module_root / "src" / f"{module}.cpp"
            cmake = module_root / "CMakeLists.txt"
            source.write_text(
                "".join(
                    f"#include <pulp/{name}/allowed.hpp>\n"
                    for name in sorted(allowed)
                )
            )
            cmake.write_text(
                f"target_link_libraries(pulp-{module} PUBLIC "
                + " ".join(f"pulp::{name}" for name in sorted(allowed))
                + ")\n"
            )
            fixtures[module] = (source, cmake)
        if verify(root):
            print("selftest valid fixture was rejected")
            return 1

        # A helper target defined in a module's build file — whose own name
        # shares the module prefix and links only in-floor libraries — is not a
        # link violation, while an out-of-floor dependency of that same helper
        # still is. Guards against reading a target name as a link.
        timeline_cmake = fixtures["timeline"][1]
        floor_links = " ".join(
            f"pulp::{name}" for name in sorted(MODULE_FLOORS["timeline"]))
        timeline_cmake.write_text(
            f"target_link_libraries(pulp-timeline PUBLIC {floor_links})\n"
            "add_executable(pulp-timeline-schema-emit tools/schema_emit_main.cpp)\n"
            "target_link_libraries(pulp-timeline-schema-emit PRIVATE pulp::timeline)\n"
        )
        if verify(root):
            print("selftest rejected in-floor helper executable target")
            return 1
        timeline_cmake.write_text(
            f"target_link_libraries(pulp-timeline PUBLIC {floor_links})\n"
            "add_executable(pulp-timeline-schema-emit tools/schema_emit_main.cpp)\n"
            "target_link_libraries(pulp-timeline-schema-emit PRIVATE pulp::render)\n"
        )
        if not any(
            "timeline outside-floor pulp::render link" in error
            for error in verify(root)
        ):
            print("selftest missed helper-target outside-floor link")
            return 1
        timeline_cmake.write_text(
            f"target_link_libraries(pulp-timeline PUBLIC {floor_links})\n")

        for module, (source, cmake) in fixtures.items():
            source.write_text("#include <pulp/render/forbidden.hpp>\n")
            errors = verify(root)
            if not any(
                f"{module} outside-floor pulp/render include" in error
                for error in errors
            ):
                print(f"selftest missed {module} render include")
                return 1
            source.write_text(f"#include <pulp/{module}/allowed.hpp>\n")

            cmake.write_text(f"target_link_libraries(pulp-{module} PUBLIC pulp::render)\n")
            errors = verify(root)
            if not any(
                f"{module} outside-floor pulp::render link" in error
                for error in errors
            ):
                print(f"selftest missed {module} namespaced render link")
                return 1

            cmake.write_text(f"target_link_libraries(pulp-{module} PUBLIC pulp-render)\n")
            if not any(
                f"{module} outside-floor pulp::render link" in error
                for error in verify(root)
            ):
                print(f"selftest missed {module} raw render link")
                return 1

            allowed = MODULE_FLOORS[module]
            cmake.write_text(
                f"target_link_libraries(pulp-{module} PUBLIC "
                + " ".join(f"pulp::{name}" for name in sorted(allowed))
                + ")\n"
            )

        shutil.rmtree(root / "core" / "timeline")
        if not any("missing required engine module" in error for error in verify(root)):
            print("selftest missed absent required timeline module")
            return 1

    print("timeline_engine_dependency_floor_selftest=true")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return run_selftest()

    errors = verify(args.repo_root.resolve())
    if errors:
        for error in errors:
            print(error)
        return 1
    print("timeline_engine_dependency_floor_verified=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
