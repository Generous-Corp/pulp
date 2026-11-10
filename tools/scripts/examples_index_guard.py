#!/usr/bin/env python3
"""Fail when examples/README.md and the examples/ directory disagree.

Every tracked directory directly under ``examples/`` must appear exactly once
in the first column of a table in ``examples/README.md``, and every directory
named there must exist. A row may name several directories (``a``, ``b``).
Rows in the proof-consumer table also name a planned location under
``test/consumers/``; the planned name must repeat the directory name, so the
list of pending moves cannot drift from the directories themselves.

It also fails when a ``NOT TARGET <name>`` guard around an example's
``add_subdirectory`` names a target that example never creates. The test tree
adds some examples (agent-toolkit-synth, sample-region-allpass) before
examples/CMakeLists.txt does, and each site guards its add_subdirectory so the
second is skipped. ``pulp_add_plugin(Foo ...)`` creates ``Foo_Core``,
``Foo_Standalone`` and friends, never a bare ``Foo``, so a guard on the bare
product name is always true: the directory is added twice and configure fails
with "add_library cannot create target ... because another target with the same
name already exists". A guarded name must be a target the directory declares
(add_library / add_executable / add_custom_target) or ``<Plugin>_<suffix>`` for a
plug-in it declares.

Usage:
    python3 tools/scripts/examples_index_guard.py [--repo-root PATH]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
README = Path("examples") / "README.md"
_ROW = re.compile(r"^\|\s*((?:`[^`|]+`(?:\s*,\s*)?)+)\s*\|(.*)\|\s*$")
_NAME = re.compile(r"`([^`]+)`")
_PLANNED = re.compile(r"`test/consumers/([^`]+)`\s*\|?\s*$")


def tracked_example_dirs(root: Path) -> set[str]:
    out = subprocess.run(["git", "ls-files", "-z", "examples"], cwd=root,
                         capture_output=True, text=True, encoding="utf-8", check=True).stdout
    dirs = set()
    for path in out.split("\0"):
        parts = path.split("/")
        if len(parts) > 2 and parts[0] == "examples":
            dirs.add(parts[1])
    return dirs


def indexed_rows(text: str) -> list[tuple[list[str], str | None]]:
    """Return (directories, planned test/consumers name or None) for each table row."""
    rows = []
    for line in text.splitlines():
        m = _ROW.match(line)
        if not m:
            continue
        names = _NAME.findall(m.group(1))
        planned = _PLANNED.search(line)
        rows.append((names, planned.group(1) if planned else None))
    return rows


GUARD_FILES = ("examples/CMakeLists.txt", "test/cmake/*.cmake", "test/CMakeLists.txt")
_IF = re.compile(r"\bif\s*\(", re.I)
_ENDIF = re.compile(r"\bendif\s*\(", re.I)
_NOT_TARGET = re.compile(r"NOT\s+TARGET\s+([A-Za-z0-9_.+:\-]+)", re.I)
_ADD_SUBDIR = re.compile(r"add_subdirectory\s*\(\s*\"?([^\s\")]+)", re.I)
_PLUGIN = re.compile(r"pulp_add_plugin\s*\(\s*([A-Za-z0-9_\-]+)", re.I)
_TARGET_DECL = re.compile(
    r"\b(?:add_library|add_executable|add_custom_target)\s*\(\s*([A-Za-z0-9_.+\-]+)", re.I)


def _strip_comments(text: str) -> str:
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def _condition_end(text: str, start: int) -> int:
    """Index just past the `)` that closes the `if(` whose `(` is at `start`."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
    return len(text)


def _block_end(text: str, after_condition: int) -> int:
    """Index past the `endif(` matching an `if(` whose condition ended at `after_condition`."""
    depth = 1
    pos = after_condition
    while depth:
        nxt_if = _IF.search(text, pos)
        nxt_end = _ENDIF.search(text, pos)
        if not nxt_end:
            return len(text)
        if nxt_if and nxt_if.start() < nxt_end.start():
            depth += 1
            pos = nxt_if.end()
        else:
            depth -= 1
            pos = nxt_end.end()
    return pos


def _resolve_dir(raw: str, root: Path, here: Path) -> Path | None:
    value = (raw.replace("${CMAKE_SOURCE_DIR}", str(root))
                .replace("${PULP_ROOT_DIR}", str(root))
                .replace("${CMAKE_CURRENT_SOURCE_DIR}", str(here.parent)))
    if "${" in value:
        return None
    path = Path(value)
    if not path.is_absolute():
        path = here.parent / path
    return path if (path / "CMakeLists.txt").is_file() else None


def _guarded_subdirs(root: Path, files: list[Path]):
    """Yield (file, guarded names, directory) for each existing add_subdirectory
    inside an `if(...)` whose condition has a `NOT TARGET` term."""
    for path in files:
        text = _strip_comments(path.read_text(encoding="utf-8"))
        for match in _IF.finditer(text):
            cond_end = _condition_end(text, match.end() - 1)
            names = _NOT_TARGET.findall(text[match.start():cond_end])
            if not names:
                continue
            body = text[cond_end:_block_end(text, cond_end)]
            for sub in _ADD_SUBDIR.findall(body):
                directory = _resolve_dir(sub, root, path)
                if directory is not None:
                    yield path, names, directory


def guard_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for pattern in GUARD_FILES:
        files.extend(sorted(root.glob(pattern)))
    return files


def guard_problems(root: Path, files: list[Path] | None = None) -> tuple[list[str], int]:
    """Problems with example add_subdirectory guards, and how many guarded
    add_subdirectory sites were examined (so an empty scan is not a pass)."""
    if files is None:
        files = guard_files(root)
    problems: list[str] = []
    examined = 0
    for path, names, directory in _guarded_subdirs(root, files):
        examined += 1
        text = _strip_comments((directory / "CMakeLists.txt").read_text(encoding="utf-8"))
        plugins = set(_PLUGIN.findall(text))
        targets = set(_TARGET_DECL.findall(text))
        for name in names:
            if name in targets or any(name.startswith(p + "_") for p in plugins):
                continue
            problems.append(
                f"{path.relative_to(root)}: `NOT TARGET {name}` guards add_subdirectory("
                f"{directory.relative_to(root)}), but that directory creates no target named "
                f"{name}; pulp_add_plugin creates <Plugin>_Core / <Plugin>_Standalone "
                f"(plug-ins: {sorted(plugins) or 'none'}). Guard on one of those.")
    return problems, examined


def check(root: Path) -> list[str]:
    readme = root / README
    if not readme.is_file():
        return [f"{README} does not exist"]
    rows = indexed_rows(readme.read_text(encoding="utf-8"))
    actual = tracked_example_dirs(root)
    problems = []
    seen: dict[str, int] = {}
    for names, planned in rows:
        for name in names:
            seen[name] = seen.get(name, 0) + 1
            if name not in actual:
                problems.append(f"{README} lists '{name}' but examples/{name}/ does not exist")
        if planned is not None and (len(names) != 1 or planned != names[0]):
            problems.append(f"{README}: planned location test/consumers/{planned} must repeat "
                            f"the directory name {names}")
    for name, count in sorted(seen.items()):
        if count > 1:
            problems.append(f"{README} lists '{name}' {count} times")
    for name in sorted(actual - set(seen)):
        problems.append(f"examples/{name}/ is not listed in {README}")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    args = ap.parse_args(argv)
    root = args.repo_root.resolve()
    problems = check(root)
    guards, examined = guard_problems(root)
    problems += guards
    for p in problems:
        print(f"examples-index-guard: {p}", file=sys.stderr)
    if problems:
        return 1
    print("examples-index-guard: examples/README.md lists every example directory once; "
          f"{examined} guarded example add_subdirectory site(s) name a target the example creates")
    return 0


if __name__ == "__main__":
    sys.exit(main())
