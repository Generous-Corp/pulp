"""Command-line entrypoint for the importer differential lab."""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import sys
from pathlib import Path

from .core import (
    LabError,
    aggregate_reports,
    compare_one,
    sha256_file,
    source_files,
    write_json,
)
from .reports import format_corpus_summary, format_summary


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def command_compare(args: argparse.Namespace) -> int:
    try:
        report = compare_one(
            args.importer.resolve(), args.observer.resolve(),
            args.file.resolve(), args.output.resolve(), args.timeout_seconds,
            args.from_source, args.browser.resolve() if args.browser else None)
    except (LabError, OSError, ValueError, json.JSONDecodeError) as exc:
        error = sanitized_error(
            exc, [args.importer.resolve(), args.observer.resolve(),
                  args.file.resolve(), args.output.resolve()])
        print(f"importer_differential_lab: {error}", file=sys.stderr)
        return 2
    print(format_summary(report))
    return 0


def command_corpus(args: argparse.Namespace) -> int:
    if args.manifest:
        manifest_path = args.manifest.resolve()
        manifest = json.loads(manifest_path.read_text())
        if manifest.get("schema") != "pulp-importer-differential-manifest-v1":
            print("importer_differential_lab: unsupported manifest schema",
                  file=sys.stderr)
            return 2
        fixtures = [
            (entry["id"], (manifest_path.parent / entry["source"]).resolve(), entry)
            for entry in manifest.get("fixtures", [])
        ]
    else:
        input_root = args.input.resolve()
        fixtures = [
            (path.relative_to(input_root).as_posix(), path, None)
            for path in source_files(input_root)]
    if not fixtures:
        print("importer_differential_lab: corpus contains no supported sources",
              file=sys.stderr)
        return 2
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    reports: list[dict] = []
    failures: list[dict[str, str]] = []
    for index, (fixture_id, source, metadata) in enumerate(fixtures, 1):
        slug = re.sub(r"[^A-Za-z0-9._-]+", "-", fixture_id)
        print(f"[{index}/{len(fixtures)}] {fixture_id}", flush=True)
        try:
            fixture_output = (
                output / "fixtures" / f"{slug}-{sha256_file(source)[:12]}")
            reports.append(compare_one(
                args.importer.resolve(), args.observer.resolve(), source,
                fixture_output, args.timeout_seconds,
                metadata.get("from", args.from_source) if metadata else args.from_source,
                args.browser.resolve() if args.browser else None, metadata))
        except (LabError, OSError, ValueError, json.JSONDecodeError) as exc:
            failures.append({
                "fixture_id": fixture_id,
                "error": sanitized_error(
                    exc, [source, output, args.importer.resolve(),
                          args.observer.resolve()]),
            })
            if args.fail_fast:
                break
    aggregate = aggregate_reports(reports, len(failures))
    aggregate["failures"] = failures
    write_json(output / "report.json", aggregate)
    (output / "summary.md").write_text(format_corpus_summary(aggregate))
    print(format_corpus_summary(aggregate))
    if failures:
        print(f"{len(failures)} fixture(s) failed; see report.json", file=sys.stderr)
    return 1 if failures else 0


def percentile(values: list[int], fraction: float) -> int:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def sanitized_error(error: Exception, paths: list[Path]) -> str:
    text = str(error)
    for path in paths:
        text = text.replace(str(path), f"<{path.name}>")
    return text


def command_benchmark(args: argparse.Namespace) -> int:
    output = args.output.resolve()
    reports = []
    try:
        for index in range(args.runs):
            print(f"[{index + 1}/{args.runs}] benchmark", flush=True)
            reports.append(compare_one(
                args.importer.resolve(), args.observer.resolve(),
                args.file.resolve(), output / "runs" / str(index + 1),
                args.timeout_seconds, args.from_source,
                args.browser.resolve() if args.browser else None))
    except (LabError, OSError, ValueError, json.JSONDecodeError) as exc:
        error = sanitized_error(
            exc, [args.importer.resolve(), args.observer.resolve(),
                  args.file.resolve(), args.output.resolve()])
        print(f"importer_differential_lab: {error}", file=sys.stderr)
        return 2
    warm = reports[1:] or reports
    browser = [report["timings"]["browser_import_ms"] for report in warm]
    native = [report["timings"]["native_total_ms"] for report in warm]
    benchmark = {
        "schema": "pulp-importer-differential-benchmark-v1",
        "version": 1,
        "runs": len(reports),
        "cold": reports[0]["timings"],
        "warm": {
            "browser_import_p50_ms": round(statistics.median(browser), 1),
            "browser_import_p95_ms": percentile(browser, 0.95),
            "native_total_p50_ms": round(statistics.median(native), 1),
            "native_total_p95_ms": percentile(native, 0.95),
            "p50_ms_saved": round(
                statistics.median(browser) - statistics.median(native), 1),
        },
    }
    write_json(output / "benchmark.json", benchmark)
    print(json.dumps(benchmark, indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Compare Pulp's native HTML importer against Chromium")
    subparsers = parser.add_subparsers(dest="command", required=True)

    def shared(subparser: argparse.ArgumentParser) -> None:
        subparser.add_argument("--importer", type=Path, required=True)
        subparser.add_argument("--observer", type=Path, required=True)
        subparser.add_argument("--browser", type=Path)
        subparser.add_argument(
            "--from", dest="from_source", default="claude",
            choices=("claude", "html", "stitch"))
        subparser.add_argument("--timeout-seconds", type=positive_int, default=60)

    compare = subparsers.add_parser("compare")
    shared(compare)
    compare.add_argument("--file", type=Path, required=True)
    compare.add_argument("--output", type=Path, required=True)
    compare.set_defaults(func=command_compare)

    corpus = subparsers.add_parser("analyze-corpus")
    shared(corpus)
    inputs = corpus.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--input", type=Path)
    inputs.add_argument("--manifest", type=Path)
    corpus.add_argument("--output", type=Path, required=True)
    corpus.add_argument("--fail-fast", action="store_true")
    corpus.set_defaults(func=command_corpus)

    benchmark = subparsers.add_parser("benchmark")
    shared(benchmark)
    benchmark.add_argument("--file", type=Path, required=True)
    benchmark.add_argument("--output", type=Path, required=True)
    benchmark.add_argument("--runs", type=positive_int, default=5)
    benchmark.set_defaults(func=command_benchmark)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    return args.func(args)
