#!/usr/bin/env python3
"""Run Swift package coverage for apple/ and stage the Codecov JSON.

This is the Apple Swift analogue of run_python_coverage.py:

- runs `swift test --enable-code-coverage` in apple/
- copies the emitted Codecov JSON into build-coverage/apple/
- writes a small text summary for the source files under apple/Sources/
"""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
APPLE_DIR = REPO_ROOT / "apple"
OUTPUT_DIR = REPO_ROOT / "build-coverage" / "apple"
JSON_FILE = OUTPUT_DIR / "coverage.apple.json"
SUMMARY_FILE = OUTPUT_DIR / "summary.txt"
SOURCE_PREFIX = "apple/Sources/"


def _run(command: list[str], *, capture_output: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=APPLE_DIR,
        check=True,
        text=True,
        capture_output=capture_output,
    )


def _codecov_report_path() -> Path:
    proc = _run(
        ["swift", "test", "--enable-code-coverage", "--show-codecov-path"],
        capture_output=True,
    )
    path = proc.stdout.strip()
    if not path:
        raise SystemExit("run_swift_coverage.py: swift test did not print a Codecov JSON path")
    return Path(path)


def _relative_repo_path(path: str) -> str | None:
    try:
        return Path(path).resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return None


def _source_entries(report: dict) -> list[tuple[str, dict]]:
    data = report.get("data", [])
    if not data:
        return []
    entries: list[tuple[str, dict]] = []
    for file_entry in data[0].get("files", []):
        relpath = _relative_repo_path(file_entry.get("filename", ""))
        if relpath is None or not relpath.startswith(SOURCE_PREFIX):
            continue
        entries.append((relpath, file_entry))
    return sorted(entries, key=lambda item: item[0])


def _format_summary(entries: list[tuple[str, dict]]) -> str:
    if not entries:
        raise ValueError("run_swift_coverage.py: no apple/Sources coverage entries found")

    total_lines = 0
    total_covered = 0
    lines = [
        "Swift apple coverage",
        f"JSON:        {JSON_FILE}",
        f"Source root: {SOURCE_PREFIX}",
        "",
        "Per-file line coverage:",
    ]
    for relpath, file_entry in entries:
        summary = file_entry["summary"]["lines"]
        covered = int(summary["covered"])
        count = int(summary["count"])
        percent = float(summary["percent"])
        total_covered += covered
        total_lines += count
        lines.append(f"  {percent:6.2f}%  {covered:4d}/{count:<4d}  {relpath}")

    overall = 100.0 if total_lines == 0 else (total_covered / total_lines) * 100.0
    lines[3:3] = [
        f"Files:       {len(entries)}",
        f"Lines:       {total_covered}/{total_lines} ({overall:.2f}%)",
    ]
    return "\n".join(lines) + "\n"


def main() -> int:
    if not APPLE_DIR.is_dir():
        raise SystemExit(f"run_swift_coverage.py: missing Swift package directory: {APPLE_DIR}")

    shutil.rmtree(OUTPUT_DIR, ignore_errors=True)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    _run(["swift", "test", "--enable-code-coverage"])
    codecov_path = _codecov_report_path()
    if not codecov_path.is_file():
        raise SystemExit(
            f"run_swift_coverage.py: reported Codecov JSON does not exist: {codecov_path}"
        )

    shutil.copy2(codecov_path, JSON_FILE)
    report = json.loads(JSON_FILE.read_text(encoding="utf-8"))
    entries = _source_entries(report)
    summary = _format_summary(entries)
    SUMMARY_FILE.write_text(summary, encoding="utf-8")
    print(summary, end="")
    print(f"Codecov JSON: {JSON_FILE}")
    print(f"Summary:      {SUMMARY_FILE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
