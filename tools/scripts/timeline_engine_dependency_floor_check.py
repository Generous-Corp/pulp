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
        for match in LINK_RE.finditer(cmake.read_text(errors="replace")):
            if match.group(1) not in allowed:
                errors.append(
                    f"{cmake.relative_to(repo_root)}: {module} outside-floor "
                    f"pulp::{match.group(1)} link"
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
