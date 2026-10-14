#!/usr/bin/env python3
"""Generate sample-region compatibility selector inputs from a real build."""

from __future__ import annotations

import argparse
import re
import shlex
from pathlib import Path


START = "COMPAT_INPUTS = ("
END = "COMPAT_FIXTURES"
TARGETS = {"compat_runner.cpp.o.d", "old_installed_sdk_consumer.cpp.o.d"}
SOURCE_RE = re.compile(r'"([^"\n]+\.(?:c|cc|cpp|cxx|mm|m|hpp|h))"')
ARCHIVE_RE = re.compile(r"(?:^|/)(?:lib)?([^/]+)\.a$")


def repo_relative(repo: Path, raw: str) -> str | None:
    try:
        return Path(raw).resolve().relative_to(repo.resolve()).as_posix()
    except ValueError:
        return None


def dependency_paths(repo: Path, build: Path) -> list[str]:
    paths: set[str] = set()
    for depfile in build.rglob("*.d"):
        if depfile.name not in TARGETS:
            continue
        words = shlex.split(depfile.read_text(encoding="utf-8").replace("\\\n", " "))
        for raw in words[1:]:
            relative = repo_relative(repo, raw)
            if relative and not relative.startswith("build"):
                paths.add(relative)

    # The real link commands identify the archives in the compatibility
    # targets. Their CMake DependInfo files contain the complete source lists
    # for those linked targets, including pulp-host and native-components.
    for target in ("sample-region-compat-runner", "sample-region-old-sdk-consumer"):
        link = next(build.rglob(f"CMakeFiles/{target}.dir/link.txt"), None)
        if not link:
            continue
        for raw in shlex.split(link.read_text(encoding="utf-8")):
            match = ARCHIVE_RE.search(raw)
            if not match:
                continue
            for info in build.rglob(f"CMakeFiles/{match.group(1)}.dir/DependInfo.cmake"):
                for source in SOURCE_RE.findall(info.read_text(encoding="utf-8")):
                    relative = repo_relative(repo, source)
                    if relative and not relative.startswith("build"):
                        paths.add(relative)

    # The installed-SDK consumer's source tree is itself a target input. This
    # is derived from the target directory, not a hand-maintained list.
    target_source = repo / "test" / "sample_region_compat"
    if target_source.is_dir():
        paths.update(p.relative_to(repo).as_posix() for p in target_source.rglob("*") if p.is_file())
    paths.update({"test/sample_region_compat/CMakeLists.txt",
                  "test/fixtures/sample-region-compat"})
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
