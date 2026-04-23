#!/usr/bin/env python3
"""Run Swift package coverage for apple/ and stage repo-relative LCOV.

This is the Apple Swift analogue of run_python_coverage.py:

- runs `swift test --enable-code-coverage` in apple/
- copies SwiftPM's LLVM coverage JSON into build-coverage/apple/ for
  local summaries/debugging
- exports LCOV from SwiftPM's profdata + test binary and rewrites every
  kept source path to repo-relative `apple/Sources/**`
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
LCOV_FILE = OUTPUT_DIR / "coverage.apple.lcov"
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


def _profdata_path(codecov_path: Path) -> Path:
    profdata_path = codecov_path.with_name("default.profdata")
    if not profdata_path.is_file():
        raise SystemExit(
            f"run_swift_coverage.py: reported profdata does not exist: {profdata_path}"
        )
    return profdata_path


def _test_binary_paths(codecov_path: Path) -> list[Path]:
    debug_dir = codecov_path.parent.parent
    binaries = sorted(
        path
        for path in debug_dir.glob("*.xctest/Contents/MacOS/*")
        if path.is_file()
    )
    if not binaries:
        raise SystemExit(
            "run_swift_coverage.py: could not find any SwiftPM test binaries under "
            f"{debug_dir}"
        )
    return binaries


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
        f"LCOV:        {LCOV_FILE}",
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


def _export_lcov(profdata_path: Path, binaries: list[Path]) -> str:
    command = [
        "xcrun",
        "llvm-cov",
        "export",
        "--format=lcov",
        "-instr-profile",
        str(profdata_path),
        *[str(binary) for binary in binaries],
    ]
    proc = _run(command, capture_output=True)
    if not proc.stdout.strip():
        raise SystemExit("run_swift_coverage.py: llvm-cov export produced no LCOV output")
    return proc.stdout


def _rewrite_lcov(lcov_text: str) -> str:
    records: list[str] = []
    current: list[str] = []
    keep_record = False

    for line in lcov_text.splitlines():
        if line.startswith("SF:"):
            relpath = _relative_repo_path(line[3:])
            keep_record = relpath is not None and relpath.startswith(SOURCE_PREFIX)
            current = [f"SF:{relpath}"] if keep_record and relpath is not None else []
            continue

        if line == "end_of_record":
            if keep_record and current:
                current.append(line)
                records.append("\n".join(current))
            current = []
            keep_record = False
            continue

        if keep_record:
            current.append(line)

    if not records:
        raise ValueError("run_swift_coverage.py: no apple/Sources LCOV records found")
    return "\n".join(records) + "\n"


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
    profdata_path = _profdata_path(codecov_path)
    test_binaries = _test_binary_paths(codecov_path)
    lcov_text = _export_lcov(profdata_path, test_binaries)
    LCOV_FILE.write_text(_rewrite_lcov(lcov_text), encoding="utf-8")
    SUMMARY_FILE.write_text(summary, encoding="utf-8")
    print(summary, end="")
    print(f"Swift JSON:   {JSON_FILE}")
    print(f"LCOV:         {LCOV_FILE}")
    print(f"Summary:      {SUMMARY_FILE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
