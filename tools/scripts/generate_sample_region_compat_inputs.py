#!/usr/bin/env python3
"""Generate the selector inputs for the sample-region compatibility probe.

The source of truth is the compiler's ``.d`` files, not a hand-maintained
header list.  Configure and build ``test/sample_region_compat`` first, then
run this script with that build directory.
"""

from __future__ import annotations

import argparse
import shlex
from pathlib import Path


START = "COMPAT_INPUTS = ("
END = "COMPAT_FIXTURES"
TARGETS = {"compat_runner.cpp.o.d", "old_installed_sdk_consumer.cpp.o.d"}


def dependency_paths(repo: Path, build: Path) -> list[str]:
    paths: set[str] = set()
    for depfile in build.rglob("*.d"):
        if depfile.name not in TARGETS:
            continue
        words = shlex.split(depfile.read_text(encoding="utf-8").replace("\\\n", " "))
        for raw in words[1:]:
            candidate = Path(raw)
            try:
                relative = candidate.resolve().relative_to(repo.resolve()).as_posix()
            except ValueError:
                continue
            if not relative.startswith("build"):
                paths.add(relative)
    paths.update({
        "test/sample_region_compat/CMakeLists.txt",
        "core/host/src/signal_graph.cpp",
        "test/fixtures/sample-region-compat",
        "test/sample_region_compat/old_graph_serializer_v2.hpp",
    })
    return sorted(paths)


def render(paths: list[str]) -> str:
    return "COMPAT_INPUTS = (\n" + "".join(f'    {path!r},\n' for path in paths) + ")\n"


def write(repo: Path, build: Path) -> int:
    target = repo / "tools/scripts/sample_region_compat_baseline.py"
    text = target.read_text(encoding="utf-8")
    begin = text.index(START)
    end = text.index(END, begin)
    generated = render(dependency_paths(repo, build))
    target.write_text(text[:begin] + generated + text[end:], encoding="utf-8")
    print(f"generated {len(dependency_paths(repo, build))} compatibility inputs")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    return write(args.repo.resolve(), args.build_dir.resolve())


if __name__ == "__main__":
    raise SystemExit(main())
