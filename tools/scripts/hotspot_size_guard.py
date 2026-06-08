#!/usr/bin/env python3
"""Guard known refactor hotspots from quietly regrowing.

The P0.1 roadmap item is intentionally simple:

* hard-fail when a tracked hotspot exceeds its frozen line-count baseline;
* warn, but do not fail, when a newly added core/tools file is already large.

The guard counts physical lines, matching the `wc -l` evidence used by the
roadmap. If a split shrinks a hotspot, lower that file's ceiling in the same
PR so future work keeps the gain.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_CONFIG = "tools/scripts/hotspot_size_guard.json"


@dataclass(frozen=True)
class Hotspot:
    path: str
    max_loc: int
    note: str = ""


@dataclass(frozen=True)
class NewFileWarning:
    max_loc: int
    paths: tuple[str, ...]


@dataclass(frozen=True)
class Config:
    hotspots: tuple[Hotspot, ...]
    new_file_warning: NewFileWarning | None


def repo_root() -> Path | None:
    result = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    return Path(result.stdout.strip())


def strip_meta(data: dict[str, Any]) -> dict[str, Any]:
    return {
        key: value for key, value in data.items()
        if not key.startswith("_") and key != "$schema"
    }


def _positive_int(value: Any, field: str) -> int:
    if not isinstance(value, int) or value <= 0:
        raise ValueError(f"{field} must be a positive integer")
    return value


def load_config(path: Path) -> Config:
    raw_data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw_data, dict):
        raise ValueError("config root must be an object")
    raw = strip_meta(raw_data)
    if raw.get("schema_version") != 1:
        raise ValueError("schema_version must be 1")

    hotspots_raw = raw.get("hotspots")
    if not isinstance(hotspots_raw, list) or not hotspots_raw:
        raise ValueError("hotspots must be a non-empty list")

    seen_paths: set[str] = set()
    hotspots: list[Hotspot] = []
    for index, entry in enumerate(hotspots_raw):
        if not isinstance(entry, dict):
            raise ValueError(f"hotspots[{index}] must be an object")
        path_value = entry.get("path")
        if not isinstance(path_value, str) or not path_value:
            raise ValueError(f"hotspots[{index}].path must be a non-empty string")
        if path_value.startswith("/") or "\\" in path_value:
            raise ValueError(f"hotspots[{index}].path must be repo-relative")
        if path_value in seen_paths:
            raise ValueError(f"duplicate hotspot path: {path_value}")
        seen_paths.add(path_value)

        note_value = entry.get("note", "")
        if not isinstance(note_value, str):
            raise ValueError(f"hotspots[{index}].note must be a string")
        hotspots.append(
            Hotspot(
                path=path_value,
                max_loc=_positive_int(entry.get("max_loc"), f"hotspots[{index}].max_loc"),
                note=note_value,
            )
        )

    warning = None
    warning_raw = raw.get("new_file_warning")
    if warning_raw is not None:
        if not isinstance(warning_raw, dict):
            raise ValueError("new_file_warning must be an object")
        paths_raw = warning_raw.get("paths")
        if not isinstance(paths_raw, list) or not paths_raw:
            raise ValueError("new_file_warning.paths must be a non-empty list")
        paths: list[str] = []
        for index, pattern in enumerate(paths_raw):
            if not isinstance(pattern, str) or not pattern:
                raise ValueError(f"new_file_warning.paths[{index}] must be a non-empty string")
            if pattern.startswith("/") or "\\" in pattern:
                raise ValueError(f"new_file_warning.paths[{index}] must be repo-relative")
            paths.append(pattern)
        warning = NewFileWarning(
            max_loc=_positive_int(warning_raw.get("max_loc"), "new_file_warning.max_loc"),
            paths=tuple(paths),
        )

    return Config(hotspots=tuple(hotspots), new_file_warning=warning)


def count_lines(path: Path) -> int:
    with path.open("rb") as handle:
        return sum(1 for _ in handle)


def added_files(base: str, head: str) -> list[str]:
    result = subprocess.run(
        ["git", "diff", "--name-status", "--diff-filter=A", f"{base}..{head}"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "git diff failed")

    out: list[str] = []
    for line in result.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) >= 2 and parts[0] == "A":
            out.append(parts[1])
    return out


def matches_any(path: str, patterns: tuple[str, ...]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def check_hotspots(root: Path, config: Config) -> tuple[list[str], list[str]]:
    failures: list[str] = []
    notes: list[str] = []
    for hotspot in config.hotspots:
        full_path = root / hotspot.path
        if not full_path.exists():
            notes.append(f"{hotspot.path}: missing from working tree; hotspot ceiling skipped")
            continue
        if not full_path.is_file():
            failures.append(f"{hotspot.path}: tracked hotspot is not a regular file")
            continue
        loc = count_lines(full_path)
        if loc > hotspot.max_loc:
            failures.append(
                f"{hotspot.path}: {loc} LOC exceeds frozen ceiling {hotspot.max_loc}"
            )
    return failures, notes


def check_new_file_warnings(root: Path, config: Config, base: str, head: str) -> list[str]:
    warning = config.new_file_warning
    if warning is None:
        return []

    warnings: list[str] = []
    for rel_path in added_files(base, head):
        if not matches_any(rel_path, warning.paths):
            continue
        full_path = root / rel_path
        if not full_path.is_file():
            continue
        loc = count_lines(full_path)
        if loc > warning.max_loc:
            warnings.append(
                f"{rel_path}: new file has {loc} LOC; consider splitting before it exceeds reviewable size"
            )
    return warnings


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="origin/main", help="git base for new-file warning checks")
    parser.add_argument("--head", default="HEAD", help="git head for new-file warning checks")
    parser.add_argument("--config", default=DEFAULT_CONFIG, help="hotspot guard config path")
    parser.add_argument(
        "--mode",
        choices=("hint", "report"),
        default="report",
        help="hint prints findings but exits 0; report fails on hard hotspot violations",
    )
    parser.add_argument(
        "--warnings-as-errors",
        action="store_true",
        help="also fail on large newly added file warnings",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    root = repo_root()
    if root is None:
        print("hotspot_size_guard: not in a git working tree", file=sys.stderr)
        return 0 if args.mode == "hint" else 2

    config_path = Path(args.config)
    if not config_path.is_absolute():
        config_path = root / config_path

    try:
        config = load_config(config_path)
        failures, notes = check_hotspots(root, config)
        warnings = check_new_file_warnings(root, config, args.base, args.head)
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"hotspot_size_guard: error: {exc}", file=sys.stderr)
        return 0 if args.mode == "hint" else 2

    if notes:
        for note in notes:
            print(f"hotspot_size_guard: note: {note}", file=sys.stderr)

    if warnings:
        print("hotspot_size_guard: large new-file warning(s):", file=sys.stderr)
        for warning in warnings:
            print(f"  {warning}", file=sys.stderr)

    if failures:
        print("hotspot_size_guard: hotspot ceiling violation(s):", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 0 if args.mode == "hint" else 1

    if warnings and args.warnings_as_errors:
        return 0 if args.mode == "hint" else 1

    print("hotspot_size_guard: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
