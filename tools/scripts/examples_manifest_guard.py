#!/usr/bin/env python3
"""Fail when docs/status/examples-manifest.json and examples/ disagree.

The manifest classifies every tracked directory directly under ``examples/``
as ``audio-only``, ``moves-to-vellum``, ``quarantined`` or ``deleted``. This
guard fails when:

- a tracked example directory has no manifest entry, or an entry names a
  directory that is not tracked;
- an entry has an unknown class, no evidence, a quarantined entry without a
  ``C4b-<platform>`` phase, or a deleted entry without a reason;
- an ``audio-only`` example links ``pulp::view``, ``pulp::render`` or
  ``pulp::canvas``, or includes a ``pulp/view``, ``pulp/render`` or
  ``pulp/canvas`` header. That is the direction that matters: UI code
  classified as audio-only would survive the cut with nothing to build it.

Usage:
    python3 tools/scripts/examples_manifest_guard.py [--repo-root PATH]
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path("docs/status/examples-manifest.json")
CLASSES = ("audio-only", "moves-to-vellum", "quarantined", "deleted")
PLATFORMS = ("cross", "macos", "ios", "android", "web")
QUARANTINE_PHASE = re.compile(r"^C4b-(ios|win|linux|android|web)$")
UI_LINK = re.compile(r"pulp::(?:view|render|canvas)\b", re.IGNORECASE)
UI_INCLUDE = re.compile(r"#\s*include\s*[<\"]pulp/(?:view|render|canvas)/")
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx", ".c++", ".c", ".hpp", ".hh", ".hxx", ".h++", ".h",
                   ".inl", ".ipp", ".tpp", ".mm", ".m", ".swift")


def tracked_files(root: Path) -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z", "examples"], cwd=root,
                         capture_output=True, text=True, encoding="utf-8", check=True).stdout
    return [p for p in out.split("\0") if p]


def tracked_example_dirs(files: list[str]) -> set[str]:
    dirs = set()
    for path in files:
        parts = path.split("/")
        if len(parts) > 2 and parts[0] == "examples":
            dirs.add(parts[1])
    return dirs


def ui_references(root: Path, files: list[str], name: str) -> tuple[list[str], list[str]]:
    """Return ('path: reference' lines, read errors) for UI-layer links and includes in one example."""
    found = []
    errors = []
    prefix = f"examples/{name}/"
    for path in files:
        if not path.startswith(prefix):
            continue
        is_cmake = path.endswith(("CMakeLists.txt", ".cmake"))
        if not (is_cmake or path.endswith(SOURCE_SUFFIXES)):
            continue
        try:
            text = (root / path).read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            errors.append(f"{path}: cannot read ({exc.__class__.__name__})")
            continue
        match = (UI_LINK if is_cmake else UI_INCLUDE).search(text)
        if match:
            found.append(f"{path}: {match.group(0).strip()}")
    return found, errors


def check(root: Path) -> list[str]:
    problems: list[str] = []
    manifest_path = root / MANIFEST
    try:
        doc = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return [f"cannot read {MANIFEST}: {exc}"]
    entries = doc.get("examples")
    if not isinstance(entries, dict):
        return [f"{MANIFEST}: 'examples' must be an object keyed by directory name"]

    files = tracked_files(root)
    dirs = tracked_example_dirs(files)
    for name in sorted(dirs - set(entries)):
        problems.append(f"examples/{name} is not classified in {MANIFEST}")
    for name in sorted(set(entries) - dirs):
        problems.append(f"{MANIFEST} classifies '{name}', which is not a tracked examples/ directory")

    for name in sorted(set(entries) & dirs):
        entry = entries[name]
        if not isinstance(entry, dict):
            problems.append(f"{name}: entry must be an object")
            continue
        cls = entry.get("class")
        if cls not in CLASSES:
            problems.append(f"{name}: class must be one of {', '.join(CLASSES)} (got {cls!r})")
            continue
        if entry.get("platform") not in PLATFORMS:
            problems.append(f"{name}: platform must be one of {', '.join(PLATFORMS)}")
        if not str(entry.get("evidence", "")).strip():
            problems.append(f"{name}: evidence is empty; say what the example links or includes")
        if cls == "quarantined" and not QUARANTINE_PHASE.match(str(entry.get("quarantine", ""))):
            problems.append(f"{name}: quarantined entries need quarantine = C4b-<ios|win|linux|android|web>")
        if cls == "deleted" and not str(entry.get("reason", "")).strip():
            problems.append(f"{name}: deleted entries need a reason")
        if cls == "audio-only":
            refs, errors = ui_references(root, files, name)
            for ref in refs:
                problems.append(f"{name}: classified audio-only but references the UI layer ({ref})")
            for err in errors:
                problems.append(f"{name}: classified audio-only but a source could not be scanned ({err})")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    args = parser.parse_args(argv)
    problems = check(args.repo_root)
    for problem in problems:
        print(f"examples-manifest: {problem}", file=sys.stderr)
    if problems:
        return 1
    print("examples-manifest: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
