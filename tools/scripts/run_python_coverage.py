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
TEST_GLOB = "tools/scripts/test_*.py"


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


def _discover_tests(pattern: str) -> list[Path]:
    tests = sorted(REPO_ROOT.glob(pattern))
    return [p for p in tests if p.is_file()]


def _write_coveragerc() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    RCFILE.write_text(
        "\n".join(
            [
                "[run]",
                "branch = True",
                "parallel = True",
                "relative_files = True",
                "source =",
                "    tools/scripts",
                "omit =",
                "    tools/scripts/test_*.py",
                "patch =",
                "    subprocess",
                "",
                "[report]",
                "show_missing = True",
                "omit =",
                "    tools/scripts/test_*.py",
                "",
                f"[html]",
                f"directory = {HTML_DIR.as_posix()}",
                "",
                "[xml]",
                f"output = {XML_FILE.as_posix()}",
                "",
            ]
        )
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
        default=TEST_GLOB,
        help=f"Glob for test discovery (default: {TEST_GLOB})",
    )
    args = parser.parse_args(argv)

    _require_supported_coverage()

    tests = _discover_tests(args.pattern)
    if not tests:
        print(f"run_python_coverage.py: no tests matched {args.pattern!r}", file=sys.stderr)
        return 1

    shutil.rmtree(OUTPUT_DIR, ignore_errors=True)
    _write_coveragerc()

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
