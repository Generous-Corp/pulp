#!/usr/bin/env python3
"""Summarize checked-in DAW-bench result manifests.

The evidence checker proves individual manifests are valid. This script builds
the recurring host-lab rollup reviewers need: which host/format runs exist,
which quirk rows were confirmed/refuted/not triggered, and which result files
back each claim.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from dataclasses import dataclass
from typing import Any

import check_daw_bench_evidence as evidence


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
DEFAULT_RESULTS_DIR = evidence.DEFAULT_RESULTS_DIR
DEFAULT_SCRIPTS_DIR = REPO_ROOT / "docs" / "validation" / "daw-bench"


@dataclass(frozen=True)
class ResultSummary:
    manifest: pathlib.Path
    date: str
    host: str
    format: str
    daw_version: str
    os: str
    result_markdown: str
    confirmed: tuple[str, ...]
    refuted: tuple[str, ...]
    not_triggered: tuple[str, ...]
    confirmed_capabilities: tuple[str, ...]


@dataclass(frozen=True)
class PlannedLane:
    host: str
    format: str
    script: pathlib.Path


def _load_manifest(path: pathlib.Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _relative(path: pathlib.Path, *, repo_root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def _quirk_flags(data: dict[str, Any], observed: str) -> tuple[str, ...]:
    quirks = data.get("quirks")
    if not isinstance(quirks, list):
        return ()
    out: list[str] = []
    for quirk in quirks:
        if isinstance(quirk, dict) and quirk.get("observed") == observed:
            flag = quirk.get("flag")
            if isinstance(flag, str):
                out.append(flag)
    return tuple(sorted(out))


def _capability_names(data: dict[str, Any], observed: str) -> tuple[str, ...]:
    capabilities = data.get("capabilities")
    if not isinstance(capabilities, list):
        return ()
    out: list[str] = []
    for capability in capabilities:
        if isinstance(capability, dict) and capability.get("observed") == observed:
            name = capability.get("capability")
            if isinstance(name, str):
                out.append(name)
    return tuple(sorted(out))


def _normalize_format(value: str) -> str:
    normalized = value.rsplit(",", 1)[-1].strip()
    compact = normalized.casefold().replace(" ", "")
    aliases = {
        "auv2": "AU",
        "au": "AU",
        "auv3": "AUv3",
        "clap": "CLAP",
        "standalone": "Standalone",
        "vst3": "VST3",
    }
    return aliases.get(compact, normalized)


def load_scripted_lanes(
    scripts_dir: pathlib.Path = DEFAULT_SCRIPTS_DIR,
    *,
    repo_root: pathlib.Path = REPO_ROOT,
) -> list[PlannedLane]:
    lanes: list[PlannedLane] = []
    for path in sorted(scripts_dir.glob("[0-9][0-9]-*.md")):
        first_line = path.read_text(encoding="utf-8").splitlines()[0].strip()
        if not first_line.startswith("# "):
            continue
        title = first_line[2:].strip()
        if "—" not in title or "(" not in title or ")" not in title:
            continue
        host_part = title.split("—", 1)[1].strip()
        host = host_part.split("(", 1)[0].strip()
        fmt = host_part.rsplit("(", 1)[1].rsplit(")", 1)[0]
        lanes.append(PlannedLane(
            host=host,
            format=_normalize_format(fmt),
            script=path.resolve().relative_to(repo_root.resolve()),
        ))
    lanes.sort(key=lambda item: (item.format.casefold(), item.host.casefold(), item.script.as_posix()))
    return lanes


def load_summaries(
    paths: list[pathlib.Path],
    *,
    repo_root: pathlib.Path = REPO_ROOT,
) -> tuple[list[ResultSummary], list[evidence.ValidationResult]]:
    manifests = evidence.find_manifests(paths)
    results = [evidence.validate_manifest(path, repo_root=repo_root) for path in manifests]
    if any(not result.ok for result in results):
        return [], results

    summaries: list[ResultSummary] = []
    for path in manifests:
        data = _load_manifest(path)
        base = path.parent
        result_markdown = data["result_markdown"]
        result_path = pathlib.Path(result_markdown)
        if not result_path.is_absolute():
            result_path = base / result_path
        summaries.append(ResultSummary(
            manifest=path,
            date=data["date"],
            host=data["host"],
            format=data["format"],
            daw_version=data["daw_version"],
            os=data["os"],
            result_markdown=_relative(result_path, repo_root=repo_root),
            confirmed=_quirk_flags(data, "Confirmed"),
            refuted=_quirk_flags(data, "Refuted"),
            not_triggered=_quirk_flags(data, "Not Triggered"),
            confirmed_capabilities=_capability_names(data, "Confirmed"),
        ))

    summaries.sort(key=lambda item: (item.date, item.host.casefold(), item.format.casefold()))
    return summaries, results


def _join_flags(flags: tuple[str, ...]) -> str:
    return ", ".join(f"`{flag}`" for flag in flags) if flags else "-"


def missing_scripted_lanes(
    summaries: list[ResultSummary],
    planned_lanes: list[PlannedLane],
) -> list[PlannedLane]:
    covered = {
        (item.host.casefold(), item.format.casefold())
        for item in summaries
    }
    return [
        lane for lane in planned_lanes
        if (lane.host.casefold(), lane.format.casefold()) not in covered
    ]


def render_markdown(
    summaries: list[ResultSummary],
    *,
    repo_root: pathlib.Path = REPO_ROOT,
    planned_lanes: list[PlannedLane] | None = None,
) -> str:
    lines: list[str] = ["# DAW Bench Compatibility Summary", ""]
    if not summaries:
        lines.extend(["No checked-in DAW-bench manifests found.", ""])
        return "\n".join(lines)

    hosts = sorted({f"{item.host} {item.format}" for item in summaries})
    latest = max(item.date for item in summaries)
    confirmed_total = sum(len(item.confirmed) for item in summaries)
    capability_total = sum(len(item.confirmed_capabilities) for item in summaries)
    lines.extend([
        f"- Manifests: {len(summaries)}",
        f"- Host/format lanes: {len(hosts)}",
        f"- Latest result date: {latest}",
        f"- Confirmed quirk observations: {confirmed_total}",
        f"- Confirmed capability observations: {capability_total}",
        "",
        "## Runs",
        "",
        "| Date | Host | Format | DAW Version | OS | Capabilities | Confirmed | Refuted | Not Triggered | Evidence |",
        "|------|------|--------|-------------|----|--------------|-----------|---------|---------------|----------|",
    ])

    for item in summaries:
        lines.append(
            f"| {item.date} | {item.host} | {item.format} | {item.daw_version} | "
            f"{item.os} | {_join_flags(item.confirmed_capabilities)} | "
            f"{_join_flags(item.confirmed)} | {_join_flags(item.refuted)} | "
            f"{_join_flags(item.not_triggered)} | `{item.result_markdown}` |"
        )

    confirmed_rows: list[tuple[str, ResultSummary]] = []
    for item in summaries:
        for flag in item.confirmed:
            confirmed_rows.append((flag, item))

    if confirmed_rows:
        lines.extend([
            "",
            "## Confirmed Quirks",
            "",
            "| Flag | Host | Format | Date | Evidence |",
            "|------|------|--------|------|----------|",
        ])
        for flag, item in sorted(confirmed_rows, key=lambda row: (row[0], row[1].host, row[1].format)):
            manifest = _relative(item.manifest, repo_root=repo_root)
            lines.append(
                f"| `{flag}` | {item.host} | {item.format} | {item.date} | `{manifest}` |"
            )

    missing_lanes = missing_scripted_lanes(summaries, planned_lanes or [])
    if missing_lanes:
        lines.extend([
            "",
            "## Scripted Lanes Without Checked-In Manifests",
            "",
            "These rows are manual bench scripts that still need real result evidence.",
            "",
            "| Host | Format | Script |",
            "|------|--------|--------|",
        ])
        for lane in missing_lanes:
            lines.append(f"| {lane.host} | {lane.format} | `{lane.script.as_posix()}` |")

    lines.append("")
    return "\n".join(lines)


def render_json(
    summaries: list[ResultSummary],
    *,
    repo_root: pathlib.Path = REPO_ROOT,
    planned_lanes: list[PlannedLane] | None = None,
) -> str:
    missing_lanes = missing_scripted_lanes(summaries, planned_lanes or [])
    data = {
        "manifest_count": len(summaries),
        "host_format_count": len({(item.host, item.format) for item in summaries}),
        "latest_result_date": max((item.date for item in summaries), default=None),
        "confirmed_quirk_observations": sum(len(item.confirmed) for item in summaries),
        "confirmed_capability_observations": sum(
            len(item.confirmed_capabilities) for item in summaries
        ),
        "runs": [
            {
                "date": item.date,
                "host": item.host,
                "format": item.format,
                "daw_version": item.daw_version,
                "os": item.os,
                "confirmed": list(item.confirmed),
                "confirmed_capabilities": list(item.confirmed_capabilities),
                "refuted": list(item.refuted),
                "not_triggered": list(item.not_triggered),
                "manifest": _relative(item.manifest, repo_root=repo_root),
                "result_markdown": item.result_markdown,
            }
            for item in summaries
        ],
        "scripted_lanes_without_manifests": [
            {
                "host": lane.host,
                "format": lane.format,
                "script": lane.script.as_posix(),
            }
            for lane in missing_lanes
        ],
    }
    return json.dumps(data, indent=2, sort_keys=True) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=pathlib.Path, default=[DEFAULT_RESULTS_DIR])
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    parser.add_argument("--require-any", action="store_true",
                        help="fail if no manifests are found")
    args = parser.parse_args(argv)

    summaries, results = load_summaries(args.paths)
    planned_lanes = load_scripted_lanes()
    if any(not result.ok for result in results):
        print(evidence.render_results(results, scanned=len(args.paths)), file=sys.stderr)
        return 1
    if not summaries and args.require_any:
        print(evidence.render_results([], scanned=len(args.paths)), file=sys.stderr)
        return 1

    if args.format == "json":
        print(render_json(summaries, planned_lanes=planned_lanes), end="")
    else:
        print(render_markdown(summaries, planned_lanes=planned_lanes), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
