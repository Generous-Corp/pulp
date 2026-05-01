#!/usr/bin/env python3
"""Run coverage.py over the first-party Python tooling test surface.

This is the Python-tooling analogue of scripts/run_coverage.sh:

- discovers the configured first-party Python tooling tests
- runs each test file under coverage.py
- enables subprocess coverage so tests that shell out to the target
  script still measure the code under test
- reports the intended first-party Python tooling roots while omitting
  test modules from the source set
- writes text, HTML, and Cobertura XML outputs for CI + local use

Run:
    python3 tools/scripts/run_python_coverage.py
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
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
    source_roots: tuple[str, ...]
    test_globs: tuple[str, ...]
    omit_globs: tuple[str, ...] = ()
    always_include: bool = False

    def resolved_omit_globs(self) -> tuple[str, ...]:
        omit_globs = self.omit_globs or self.test_globs
        return omit_globs + tuple(f"{source_root}/_*.py" for source_root in self.source_roots)


COVERAGE_SURFACES = (
    CoverageSurface(("tools/scripts",), ("tools/scripts/test_*.py",)),
    CoverageSurface(("tools/deps",), ("tools/deps/test_*.py",)),
    CoverageSurface(("tools/local-ci",), ("tools/local-ci/test_*.py",)),
    # Keep the broader first-party tooling roots represented while the
    # executed test set stays on the established tooling roots plus
    # targeted top-level tooling tests.
    CoverageSurface(
        ("tools", "core/view/js"),
        (
            "tools/test_check_format_validation.py",
            "tools/test_check_status_ladder.py",
            "tools/test_list_limitations.py",
        ),
        (
            "tools/test_*.py",
            "tools/scripts/test_*.py",
            "tools/deps/test_*.py",
            "tools/local-ci/test_*.py",
            "tools/packages/test_*.py",
            "tools/scripts/_*.py",
            "tools/deps/_*.py",
            "tools/local-ci/_*.py",
            "tools/packages/_*.py",
            # tools/sandbox-e2e/ is end-to-end CLI test harness
            # (pulp#732). It's invoked by .github/workflows/sandbox-e2e.yml
            # against real binaries and doesn't participate in this
            # Python-tooling coverage lane. Mirror codecov.yml's
            # ignore entry for tools/sandbox-e2e/**.
            "tools/sandbox-e2e/*.py",
            "tools/sandbox-e2e/**/*.py",
        ),
        always_include=True,
    ),
)
DEFAULT_TEST_GLOBS = list(
    dict.fromkeys(pattern for surface in COVERAGE_SURFACES for pattern in surface.test_globs)
)


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


def _dedupe(items: list[str]) -> list[str]:
    seen: set[str] = set()
    deduped: list[str] = []
    for item in items:
        if item in seen:
            continue
        seen.add(item)
        deduped.append(item)
    return deduped


def _selected_surfaces(tests: list[Path]) -> list[CoverageSurface]:
    rel_paths = [test.relative_to(REPO_ROOT).as_posix() for test in tests]
    selected: list[CoverageSurface] = []
    for surface in COVERAGE_SURFACES:
        if surface.test_globs and any(
            fnmatch.fnmatch(rel_path, pattern)
            for rel_path in rel_paths
            for pattern in surface.test_globs
        ):
            selected.append(surface)
    for surface in COVERAGE_SURFACES:
        if surface.always_include and surface not in selected:
            selected.append(surface)
    return selected


def _has_selected_test_surface(tests: list[Path]) -> bool:
    rel_paths = [test.relative_to(REPO_ROOT).as_posix() for test in tests]
    return any(
        surface.test_globs and any(
            fnmatch.fnmatch(rel_path, pattern)
            for rel_path in rel_paths
            for pattern in surface.test_globs
        )
        for surface in COVERAGE_SURFACES
    )


def _normalized_source_roots(surfaces: list[CoverageSurface]) -> list[str]:
    roots = _dedupe(
        [source_root for surface in surfaces for source_root in surface.source_roots]
    )
    normalized: list[str] = []
    for root in sorted(roots, key=lambda value: (value.count("/"), value)):
        if any(root.startswith(f"{existing}/") for existing in normalized):
            continue
        normalized.append(root)
    return normalized


def _resolved_omit_globs(surfaces: list[CoverageSurface]) -> list[str]:
    return _dedupe(
        [
            omit_glob
            for surface in surfaces
            for omit_glob in surface.resolved_omit_globs()
        ]
    )


def _matches_any_glob(rel_path: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatch(rel_path, pattern) for pattern in patterns)


def _report_source_files(source_roots: list[str], omit_globs: list[str]) -> list[Path]:
    files: list[Path] = []
    seen: set[Path] = set()
    for source_root in source_roots:
        root = REPO_ROOT / source_root
        candidates = [root] if root.is_file() else sorted(root.rglob("*.py"))
        for path in candidates:
            if not path.is_file() or path.suffix != ".py":
                continue
            rel_path = path.relative_to(REPO_ROOT).as_posix()
            if _matches_any_glob(rel_path, omit_globs):
                continue
            if path in seen:
                continue
            seen.add(path)
            files.append(path)
    return files


def _touch_report_source_files(
    cov: coverage.Coverage,
    source_roots: list[str],
    omit_globs: list[str],
) -> None:
    data = cov.get_data()
    for path in _report_source_files(source_roots, omit_globs):
        data.touch_file(path.relative_to(REPO_ROOT).as_posix())


def _repo_relative_xml_filename(filename: str, sources: list[str]) -> str:
    normalized = filename.replace("\\", "/")
    candidate = Path(normalized)
    if candidate.is_absolute():
        try:
            return candidate.resolve().relative_to(REPO_ROOT.resolve()).as_posix()
        except ValueError:
            return normalized

    if (REPO_ROOT / normalized).exists():
        return normalized

    for source in sources:
        source_path = Path(source or ".")
        if source_path.is_absolute():
            full_path = source_path / normalized
            try:
                return full_path.resolve().relative_to(REPO_ROOT.resolve()).as_posix()
            except ValueError:
                continue
        rel_path = (source_path / normalized).as_posix()
        if (REPO_ROOT / rel_path).exists():
            return rel_path

    return normalized


def _rewrite_cobertura_filenames(xml_file: Path) -> None:
    tree = ET.parse(xml_file)
    root = tree.getroot()
    sources_element = root.find("sources")
    sources = [
        source.text or "."
        for source in root.findall("./sources/source")
    ]
    for class_element in root.findall(".//class"):
        filename = class_element.get("filename")
        if not filename:
            continue
        class_element.set("filename", _repo_relative_xml_filename(filename, sources))

    if sources_element is not None:
        sources_element.clear()
        ET.SubElement(sources_element, "source").text = "."

    tree.write(xml_file, encoding="utf-8", xml_declaration=True)


def _write_coveragerc(surfaces: list[CoverageSurface]) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    source_roots = _normalized_source_roots(surfaces)
    omit_globs = _resolved_omit_globs(surfaces)
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


def _build_reports(surfaces: list[CoverageSurface]) -> None:
    source_roots = _normalized_source_roots(surfaces)
    omit_globs = _resolved_omit_globs(surfaces)
    cov = coverage.Coverage(config_file=str(RCFILE), data_file=str(DATA_FILE))
    cov.combine(data_paths=[str(OUTPUT_DIR)], strict=True)
    _touch_report_source_files(cov, source_roots, omit_globs)
    cov.save()

    with SUMMARY_FILE.open("w", encoding="utf-8") as fh:
        cov.report(file=fh, show_missing=True)
    print(SUMMARY_FILE.read_text(encoding="utf-8"), end="")
    cov.html_report(directory=str(HTML_DIR))
    cov.xml_report(outfile=str(XML_FILE))
    _rewrite_cobertura_filenames(XML_FILE)


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

    if not _has_selected_test_surface(tests):
        print(
            "run_python_coverage.py: matched tests are outside the configured "
            "Python coverage surfaces",
            file=sys.stderr,
        )
        return 1
    surfaces = _selected_surfaces(tests)

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
        _build_reports(surfaces)
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
