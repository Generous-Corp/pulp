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
import re
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


@dataclass(frozen=True)
class HotspotShrink:
    path: str
    base_loc: int
    current_loc: int
    base_max_loc: int
    current_max_loc: int | None

    @property
    def ceiling_lowered(self) -> bool:
        if self.current_max_loc is None:
            return False
        return self.current_max_loc < self.base_max_loc

    @property
    def ceiling_tracks_current_loc(self) -> bool:
        if self.current_max_loc is None:
            return False
        return self.current_max_loc == self.current_loc

    @property
    def ratchet_possible(self) -> bool:
        """True when the shrink leaves the file strictly under its ceiling.

        A hotspot can sit ABOVE its ceiling: `check_hotspots` only fails a PR
        that itself grew the file, so main can drift past `max_loc` while every
        individual PR stays net-neutral. Shrinking such a file cannot lower the
        ceiling — setting `max_loc` to the new LOC would RAISE it — so demanding
        a reduction there is unsatisfiable, and the PR gets trapped between
        `ceiling_lowered` and `ceiling_tracks_current_loc`. Only ask for the
        ratchet when there is headroom to reclaim.
        """
        return self.current_loc < self.base_max_loc


@dataclass(frozen=True)
class HotspotRemoval:
    path: str
    current_loc: int
    base_max_loc: int


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


def parse_config(raw_data: Any) -> Config:
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


def load_config(path: Path) -> Config:
    return parse_config(json.loads(path.read_text(encoding="utf-8")))


def count_lines(path: Path) -> int:
    with path.open("rb") as handle:
        return sum(1 for _ in handle)


def count_blob_lines(data: bytes) -> int:
    if not data:
        return 0
    return data.count(b"\n") + (0 if data.endswith(b"\n") else 1)


def repo_relative_path(root: Path, path: Path) -> str | None:
    try:
        rel = path.resolve().relative_to(root.resolve())
    except ValueError:
        return None
    return rel.as_posix()


def git_show_bytes(ref: str, rel_path: str) -> bytes | None:
    result = subprocess.run(
        ["git", "show", f"{ref}:{rel_path}"],
        capture_output=True,
    )
    if result.returncode != 0:
        return None
    return result.stdout


def merge_base(base: str, head: str) -> str | None:
    result = subprocess.run(
        ["git", "merge-base", base, head],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def config_at_ref(ref: str, rel_path: str) -> Config | None:
    raw = git_show_bytes(ref, rel_path)
    if raw is None:
        return None
    try:
        return parse_config(json.loads(raw.decode("utf-8")))
    except (UnicodeDecodeError, ValueError, json.JSONDecodeError):
        return None


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


def changed_files(base: str, head: str) -> set[str]:
    result = subprocess.run(
        ["git", "diff", "--name-only", f"{base}..{head}"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "git diff failed")
    return {line for line in result.stdout.splitlines() if line}


def matches_any(path: str, patterns: tuple[str, ...]) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def hotspot_grow_overrides(base: str, head: str) -> frozenset[str]:
    """Paths authorized to grow via a `Hotspot-Grow: <path|all>` trailer.

    Deliberate hotspot growth declares itself per-PR with a trailer instead of
    bumping the shared `max_loc` counter in the config (which every growing PR
    would edit → the same O(N^2) conflict as the version line). Collected across
    the whole range so a trailer on any commit counts.
    """
    result = subprocess.run(
        ["git", "log", "--format=%B%x00", f"{base}..{head}"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        return frozenset()
    paths: set[str] = set()
    for line in result.stdout.splitlines():
        m = re.match(r"\s*Hotspot-Grow:\s*(\S+)", line)
        if m:
            paths.add(m.group(1))
    return frozenset(paths)


def check_hotspots(
    root: Path,
    config: Config,
    base: str | None = None,
    head: str | None = None,
    grow_overrides: frozenset[str] = frozenset(),
) -> tuple[list[str], list[str]]:
    """Fail a PR that grows a frozen hotspot past its reference size.

    Rebase-stable: `max_loc` is a *reference* floor, not a shared counter every
    growing PR must bump. A file over its reference is a violation only when
    THIS PR grew it there — measured as ``head_loc > merge_base_loc`` — and no
    `Hotspot-Grow:` trailer authorizes it. So a net-neutral change passes even
    while main grows the same file (killing the ceiling-bump race), and
    deliberate growth uses a per-PR trailer instead of editing the config.

    When ``base``/``head`` are not given (or no merge-base resolves) the check
    falls back to the pre-net-delta absolute behavior for backward compatibility.
    """
    failures: list[str] = []
    notes: list[str] = []
    mb = merge_base(base, head) if base and head else None
    for hotspot in config.hotspots:
        full_path = root / hotspot.path
        if not full_path.exists():
            notes.append(f"{hotspot.path}: missing from working tree; hotspot ceiling skipped")
            continue
        if not full_path.is_file():
            failures.append(f"{hotspot.path}: tracked hotspot is not a regular file")
            continue
        loc = count_lines(full_path)
        if loc <= hotspot.max_loc:
            continue
        if mb is None:
            failures.append(
                f"{hotspot.path}: {loc} LOC exceeds frozen ceiling {hotspot.max_loc}"
            )
            continue
        mb_bytes = git_show_bytes(mb, hotspot.path)
        mb_loc = count_blob_lines(mb_bytes) if mb_bytes is not None else 0
        if loc <= mb_loc:
            notes.append(
                f"{hotspot.path}: {loc} LOC over reference {hotspot.max_loc}, but this "
                f"PR did not grow it (merge-base {mb_loc}); not a violation"
            )
            continue
        if hotspot.path in grow_overrides or "all" in grow_overrides:
            notes.append(
                f"{hotspot.path}: grows {mb_loc} -> {loc} LOC, authorized by Hotspot-Grow"
            )
            continue
        failures.append(
            f"{hotspot.path}: this PR grows a frozen hotspot {mb_loc} -> {loc} LOC "
            f"(> reference {hotspot.max_loc}); make the change net-neutral or add a "
            f"`Hotspot-Grow: {hotspot.path} reason=\"...\"` trailer"
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


def git_tracked_files(patterns: tuple[str, ...]) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", *patterns],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "git ls-files failed")
    return [line for line in result.stdout.splitlines() if line]


def unconfigured_large_files(root: Path, config: Config, limit: int = 10) -> list[tuple[str, int]]:
    warning = config.new_file_warning
    if warning is None:
        return []

    tracked_hotspots = {hotspot.path for hotspot in config.hotspots}
    large_files: list[tuple[str, int]] = []
    for rel_path in git_tracked_files(warning.paths):
        if rel_path in tracked_hotspots:
            continue
        full_path = root / rel_path
        if not full_path.is_file():
            continue
        if full_path.suffix not in LARGE_FILE_REPORT_EXTENSIONS:
            continue
        loc = count_lines(full_path)
        if loc > warning.max_loc:
            large_files.append((rel_path, loc))
    return sorted(large_files, key=lambda item: (-item[1], item[0]))[:limit]


NOTE_HISTORY_PATTERN = re.compile(
    r"\b(PR|issue|Phase|phase|branch|slice)\b|#[0-9]+|[0-9]{4}-[0-9]{2}-[0-9]{2}"
)
LARGE_FILE_REPORT_EXTENSIONS = {
    ".c",
    ".cc",
    ".cmake",
    ".cpp",
    ".h",
    ".hpp",
    ".js",
    ".m",
    ".md",
    ".mm",
    ".py",
    ".rs",
    ".sh",
    ".swift",
    ".ts",
    ".tsx",
}


def historical_note_warnings(config: Config) -> list[str]:
    warnings: list[str] = []
    for hotspot in config.hotspots:
        if hotspot.note and NOTE_HISTORY_PATTERN.search(hotspot.note):
            warnings.append(f"{hotspot.path}: note may contain stale context: {hotspot.note}")
    return warnings


def hotspot_shrinks(
    current: Config,
    base: str,
    head: str,
    config_rel_path: str,
) -> tuple[list[HotspotShrink], list[HotspotRemoval], list[str]]:
    comparison_base = merge_base(base, head)
    if comparison_base is None:
        return [], [], [f"could not find merge-base for {base} and {head}; shrink check skipped"]
    base_config = config_at_ref(comparison_base, config_rel_path)
    if base_config is None:
        return [], [], [
            f"could not read base hotspot config at {comparison_base}:{config_rel_path}; shrink check skipped"
        ]

    base_hotspots = {hotspot.path: hotspot for hotspot in base_config.hotspots}
    current_hotspots = {hotspot.path: hotspot for hotspot in current.hotspots}
    touched = changed_files(comparison_base, head)
    shrinks: list[HotspotShrink] = []
    removals: list[HotspotRemoval] = []

    for path, base_hotspot in base_hotspots.items():
        current_hotspot = current_hotspots.get(path)
        if current_hotspot is None:
            current_blob = git_show_bytes(head, path)
            if current_blob is not None:
                removals.append(
                    HotspotRemoval(
                        path=path,
                        current_loc=count_blob_lines(current_blob),
                        base_max_loc=base_hotspot.max_loc,
                    )
                )
            continue
        if path not in touched:
            continue
        base_blob = git_show_bytes(comparison_base, path)
        current_blob = git_show_bytes(head, path)
        if base_blob is None or current_blob is None:
            continue
        base_loc = count_blob_lines(base_blob)
        current_loc = count_blob_lines(current_blob)
        if current_loc < base_loc:
            shrinks.append(
                HotspotShrink(
                    path=path,
                    base_loc=base_loc,
                    current_loc=current_loc,
                    base_max_loc=base_hotspot.max_loc,
                    current_max_loc=current_hotspot.max_loc if current_hotspot is not None else None,
                )
            )

    return shrinks, removals, []


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
    parser.add_argument(
        "--require-ceiling-reduction",
        action="store_true",
        help="fail when a changed tracked hotspot shrinks but its max_loc ceiling does not ratchet to the new LOC",
    )
    parser.add_argument(
        "--show-inventory",
        action="store_true",
        help="print broad large-file and hotspot-note inventory hints; noisy by design, so not used by required gates",
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
    config_rel_path = repo_relative_path(root, config_path)

    try:
        if config_rel_path is None:
            raise ValueError("config path must be inside the repository for shrink checks")
        config = load_config(config_path)
        failures, notes = check_hotspots(
            root, config, args.base, args.head,
            hotspot_grow_overrides(args.base, args.head),
        )
        warnings = check_new_file_warnings(root, config, args.base, args.head)
        shrinks, removals, shrink_notes = hotspot_shrinks(
            config, args.base, args.head, config_rel_path
        )
        if args.show_inventory:
            large_unconfigured = unconfigured_large_files(root, config)
            note_warnings = historical_note_warnings(config)
        else:
            large_unconfigured = []
            note_warnings = []
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"hotspot_size_guard: error: {exc}", file=sys.stderr)
        return 0 if args.mode == "hint" else 2

    if notes:
        for note in notes:
            print(f"hotspot_size_guard: note: {note}", file=sys.stderr)
    if shrink_notes:
        for note in shrink_notes:
            print(f"hotspot_size_guard: note: {note}", file=sys.stderr)

    if warnings:
        print("hotspot_size_guard: large new-file warning(s):", file=sys.stderr)
        for warning in warnings:
            print(f"  {warning}", file=sys.stderr)

    if shrinks:
        print("hotspot_size_guard: tracked hotspot shrink(s):", file=sys.stderr)
        for shrink in shrinks:
            if shrink.current_max_loc is None:
                state = "ceiling removed"
                current_max_loc = "removed"
            elif shrink.ceiling_lowered:
                state = "ceiling lowered"
                current_max_loc = str(shrink.current_max_loc)
            elif not shrink.ratchet_possible:
                state = "still at/over ceiling; no reduction to make"
                current_max_loc = str(shrink.current_max_loc)
            else:
                state = "ceiling unchanged"
                current_max_loc = str(shrink.current_max_loc)
            print(
                "  "
                f"{shrink.path}: {shrink.base_loc} -> {shrink.current_loc} LOC; "
                f"max_loc {shrink.base_max_loc} -> {current_max_loc} ({state})",
                file=sys.stderr,
            )

    if removals:
        print("hotspot_size_guard: tracked hotspot removal(s):", file=sys.stderr)
        for removal in removals:
            print(
                "  "
                f"{removal.path}: removed from config but still exists at "
                f"{removal.current_loc} LOC; previous max_loc {removal.base_max_loc}",
                file=sys.stderr,
            )

    if large_unconfigured:
        print("hotspot_size_guard: largest unconfigured large file(s):", file=sys.stderr)
        for rel_path, loc in large_unconfigured:
            print(f"  {rel_path}: {loc} LOC exceeds warning threshold", file=sys.stderr)

    if note_warnings:
        print("hotspot_size_guard: hotspot note context warning(s):", file=sys.stderr)
        for warning in note_warnings:
            print(f"  {warning}", file=sys.stderr)

    if failures:
        print("hotspot_size_guard: hotspot growth violation(s):", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 0 if args.mode == "hint" else 1

    if warnings and args.warnings_as_errors:
        return 0 if args.mode == "hint" else 1

    if args.require_ceiling_reduction:
        if removals:
            print("hotspot_size_guard: missing hotspot ceiling entry removal(s):", file=sys.stderr)
            for removal in removals:
                print(
                    "  "
                    f"{removal.path}: restore its entry in {config_rel_path} with "
                    f"max_loc {removal.current_loc}, or delete the file in the same PR",
                    file=sys.stderr,
                )
            return 0 if args.mode == "hint" else 1

        missing_reductions = [
            shrink for shrink in shrinks
            if shrink.ratchet_possible
            and (not shrink.ceiling_lowered or not shrink.ceiling_tracks_current_loc)
        ]
        if missing_reductions:
            print("hotspot_size_guard: missing hotspot ceiling reduction(s):", file=sys.stderr)
            for shrink in missing_reductions:
                if shrink.current_max_loc is None:
                    current_state = "removed"
                else:
                    current_state = str(shrink.current_max_loc)
                print(
                    "  "
                    f"{shrink.path}: shrank {shrink.base_loc} -> {shrink.current_loc} LOC "
                    f"but max_loc is {current_state}; set max_loc to "
                    f"{shrink.current_loc} in {config_rel_path}",
                    file=sys.stderr,
                )
            return 0 if args.mode == "hint" else 1

    print("hotspot_size_guard: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
