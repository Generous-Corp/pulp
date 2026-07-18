#!/usr/bin/env python3
"""Keep core/timeline below format, host, view, render, and product layers."""

from __future__ import annotations

import argparse
import re
import tempfile
from pathlib import Path


ALLOWED = {"timeline", "timebase", "platform", "runtime", "audio", "midi"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]pulp/([^/]+)/', re.MULTILINE)
LINK_RE = re.compile(r"\bpulp(?:::|-)([a-zA-Z0-9_-]+)\b")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}


def verify(root: Path) -> list[str]:
    module = root / "core" / "timeline"
    if not module.is_dir():
        return [f"missing timeline module: {module}"]
    errors: list[str] = []
    for path in sorted(module.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        for match in INCLUDE_RE.finditer(path.read_text(errors="replace")):
            if match.group(1) not in ALLOWED:
                errors.append(
                    f"{path.relative_to(root)}: outside-floor pulp/{match.group(1)} include"
                )
    cmake = module / "CMakeLists.txt"
    for match in LINK_RE.finditer(cmake.read_text(errors="replace")):
        if match.group(1) not in ALLOWED:
            errors.append(
                f"{cmake.relative_to(root)}: outside-floor pulp::{match.group(1)} link"
            )
    return errors


def selftest() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        source = root / "core" / "timeline" / "src" / "timeline.cpp"
        source.parent.mkdir(parents=True)
        source.write_text("#include <pulp/runtime/result.hpp>\n")
        cmake = root / "core" / "timeline" / "CMakeLists.txt"
        cmake.write_text("target_link_libraries(pulp-timeline PUBLIC pulp::runtime)\n")
        if verify(root):
            print("valid fixture rejected")
            return 1
        source.write_text("#include <pulp/format/processor.hpp>\n")
        if not any("pulp/format include" in error for error in verify(root)):
            print("forbidden include missed")
            return 1
        source.write_text("#include <pulp/runtime/result.hpp>\n")
        cmake.write_text("target_link_libraries(pulp-timeline PUBLIC pulp::host)\n")
        if not any("pulp::host link" in error for error in verify(root)):
            print("forbidden link missed")
            return 1
    print("timeline_dependency_floor_selftest=true")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path,
                        default=Path(__file__).resolve().parents[2])
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    errors = verify(args.repo_root.resolve())
    if errors:
        print("\n".join(errors))
        return 1
    print("timeline_dependency_floor_verified=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
