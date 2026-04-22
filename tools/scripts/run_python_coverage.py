#!/usr/bin/env python3
"""Run coverage.py over the tools/scripts Python test surface.

This is the Python-tooling analogue of scripts/run_coverage.sh:

- discovers `tools/scripts/test_*.py`
- runs each test file under coverage.py
- enables subprocess coverage so tests that shell out to the target
  script still measure the code under test
- writes text, HTML, and Cobertura XML outputs for CI + local use

Run:
    python3 tools/scripts/run_python_coverage.py
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    import coverage
except ImportError:  # pragma: no cover - exercised manually
    coverage = None


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
OUTPUT_DIR = REPO_ROOT / "build-coverage" / "python"
HTML_DIR = OUTPUT_DIR / "html"
SUMMARY_FILE = OUTPUT_DIR / "summary.txt"
XML_FILE = OUTPUT_DIR / "coverage.python.xml"
DATA_FILE = OUTPUT_DIR / ".coverage"
RCFILE = OUTPUT_DIR / ".coveragerc"


@dataclass(frozen=True)
class CoverageSurface:
    source_root: str
    test_glob: str


COVERAGE_SURFACES = (
    CoverageSurface("tools/scripts", "tools/scripts/test_*.py"),
    CoverageSurface("tools/deps", "tools/deps/test_*.py"),
    CoverageSurface("tools/local-ci", "tools/local-ci/test_*.py"),
)
DEFAULT_TEST_GLOBS = [surface.test_glob for surface in COVERAGE_SURFACES]


def _require_supported_coverage() -> None:
    if coverage is None:
        raise SystemExit(
            "run_python_coverage.py requires coverage.py >= 7.10.\n"
            "Install it with:\n"
            "  python3 -m pip install 'coverage>=7.10'"
        )
    match = re.match(r"^(\d+)\.(\d+)", coverage.__version__)
    if not match:
        return
    version = (int(match.group(1)), int(match.group(2)))
    if version < (7, 10):
        raise SystemExit(
            f"coverage.py {coverage.__version__} is too old; "
            "need >= 7.10 for [run] patch = subprocess"
        )


def _discover_tests(patterns: list[str]) -> list[Path]:
    seen: set[Path] = set()
    tests: list[Path] = []
    for pattern in patterns:
        for path in sorted(REPO_ROOT.glob(pattern)):
            if not path.is_file() or path in seen:
                continue
            seen.add(path)
            tests.append(path)
    return tests


def _selected_surfaces(tests: list[Path]) -> list[CoverageSurface]:
    selected: list[CoverageSurface] = []
    for surface in COVERAGE_SURFACES:
        prefix = f"{surface.source_root}/"
        if any(test.relative_to(REPO_ROOT).as_posix().startswith(prefix) for test in tests):
            selected.append(surface)
    return selected


def _write_coveragerc(surfaces: list[CoverageSurface]) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    source_roots = [surface.source_root for surface in surfaces]
    omit_globs: list[str] = []
    for surface in surfaces:
        omit_globs.append(surface.test_glob)
        omit_globs.append(f"{surface.source_root}/_*.py")
    if not source_roots:
        raise ValueError("run_python_coverage.py: no coverage surfaces selected")

    lines = [
        "[run]",
        "branch = True",
        "parallel = True",
        "relative_files = True",
        "source =",
    ]
    lines.extend(f"    {source_root}" for source_root in source_roots)
    lines.extend(
        [
            "omit =",
        ]
    )
    lines.extend(f"    {omit_glob}" for omit_glob in omit_globs)
    lines.extend(
        [
            "patch =",
            "    subprocess",
            "",
            "[report]",
            "show_missing = True",
            "omit =",
        ]
    )
    lines.extend(f"    {omit_glob}" for omit_glob in omit_globs)
    lines.extend(
        [
            "",
            "[html]",
            f"directory = {HTML_DIR.as_posix()}",
            "",
            "[xml]",
            f"output = {XML_FILE.as_posix()}",
            "",
        ]
    )
    RCFILE.write_text(
        "\n".join(lines)
        + "\n",
        encoding="utf-8",
    )


def _run_test(test_path: Path, env: dict[str, str]) -> int:
    rel = test_path.relative_to(REPO_ROOT)
    print(f"=== Python coverage: {rel} ===", flush=True)
    proc = subprocess.run(
        [
            sys.executable,
            "-m",
            "coverage",
            "run",
            "--rcfile",
            str(RCFILE),
            "--parallel-mode",
            str(rel),
        ],
        cwd=REPO_ROOT,
        env=env,
    )
    return proc.returncode


def _build_reports() -> None:
    cov = coverage.Coverage(config_file=str(RCFILE), data_file=str(DATA_FILE))
    cov.combine(data_paths=[str(OUTPUT_DIR)], strict=True)
    cov.save()

    with SUMMARY_FILE.open("w", encoding="utf-8") as fh:
        cov.report(file=fh, show_missing=True)
    print(SUMMARY_FILE.read_text(encoding="utf-8"), end="")
    cov.html_report(directory=str(HTML_DIR))
    cov.xml_report(outfile=str(XML_FILE))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--pattern",
        action="append",
        default=None,
        help=(
            "Repeatable glob for test discovery. Defaults to the configured "
            "Python coverage surfaces."
        ),
    )
    args = parser.parse_args(argv)

    _require_supported_coverage()

    patterns = args.pattern or DEFAULT_TEST_GLOBS
    tests = _discover_tests(patterns)
    if not tests:
        print(f"run_python_coverage.py: no tests matched {patterns!r}", file=sys.stderr)
        return 1

    surfaces = _selected_surfaces(tests)
    if not surfaces:
        print(
            "run_python_coverage.py: matched tests are outside the configured "
            "Python coverage surfaces",
            file=sys.stderr,
        )
        return 1

    shutil.rmtree(OUTPUT_DIR, ignore_errors=True)
    _write_coveragerc(surfaces)

    env = os.environ.copy()
    env["COVERAGE_PROCESS_START"] = str(RCFILE)
    env["COVERAGE_FILE"] = str(DATA_FILE)

    failures: list[str] = []
    for test_path in tests:
        rc = _run_test(test_path, env)
        if rc != 0:
            failures.append(f"{test_path.relative_to(REPO_ROOT)} (exit {rc})")

    try:
        _build_reports()
    except coverage.exceptions.NoDataError:
        print("run_python_coverage.py: coverage.py produced no data", file=sys.stderr)
        return 1

    print(f"HTML report: {HTML_DIR / 'index.html'}")
    print(f"Summary:     {SUMMARY_FILE}")
    print(f"Cobertura:   {XML_FILE}")

    if failures:
        print("", file=sys.stderr)
        print(
            "=== FAIL: one or more Python coverage tests exited non-zero; "
            "coverage above is based on partial data. ===",
            file=sys.stderr,
        )
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
