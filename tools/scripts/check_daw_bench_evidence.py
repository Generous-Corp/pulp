#!/usr/bin/env python3
"""Validate checked-in DAW-bench evidence manifests.

DAW-bench sessions are manual, but their durable result records should still be
machine-checkable before they are used to promote host-quirk tiers or justify
roadmap status. This script validates JSON manifests stored under
``docs/validation/daw-bench/results/YYYY-MM-DD/``.

Accepted manifest filenames end in ``.daw-bench.json``. The default directory
scan is advisory when no manifests exist so the checker can land before the
first real lab run. Use ``--require-any`` in a promotion PR that claims fresh
bench evidence.
"""

from __future__ import annotations

import argparse
import csv
import datetime as _dt
import json
import pathlib
import re
import sys
from dataclasses import dataclass
from typing import Any


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
DEFAULT_RESULTS_DIR = REPO_ROOT / "docs" / "validation" / "daw-bench" / "results"
MANIFEST_SUFFIX = ".daw-bench.json"
SCHEMA_VERSION = 1

ALLOWED_FORMATS = {"AU", "AUv3", "CLAP", "Standalone", "VST3"}
ALLOWED_RESULTS = {"Confirmed", "Refuted", "Not Triggered"}
REQUIRED_STRING_FIELDS = (
    "host",
    "format",
    "daw_version",
    "os",
    "date",
    "script",
    "pulp_commit",
    "plugin_version",
    "result_markdown",
)
PLACEHOLDER_RE = re.compile(r"(?:^|\b)(?:TBD|TODO|FIXME|<[^>]+>|paste here)(?:\b|$)", re.I)
COMMIT_RE = re.compile(r"^[0-9a-f]{7,40}$")
FLAG_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
LOG_LINE_RE = re.compile(r"^[^\t]+\t(?P<event>[A-Za-z_][A-Za-z0-9_]*)")
LOG_PLUGIN_VERSION_KEYS = {"plugin_version", "pulp_bench_plugin"}

FLAG_LOG_EVENTS: dict[str, tuple[str, ...]] = {
    "clamp_latency_to_nonneg": ("prepare",),
    "reaper_vst3_gesture_ordering": ("process_is_playing_edge",),
    "reaper_process_while_bypassed": ("process_without_prepare",),
    "reaper_permissive_bus_arrangements": ("bus_layout_proposal",),
    "reaper_clap_transport_edges": ("process_is_playing_edge",),
    "reaper_anticipative_fx_buffer_variability": (
        "process_buffer_overrun",
        "process_sample_rate_drift",
    ),
    "reaper_midsession_setstate": ("deserialize_plugin_state",),
    "fl_studio_setactive_process_mutex": ("process_without_prepare",),
    "fl_studio_state_reader_skip": ("deserialize_plugin_state",),
    "cubase9_state_blob_size_validation": ("deserialize_plugin_state",),
    "cubase10_async_view_resize_queue": ("view_resized",),
    "live_vst3_canresize_ignore": ("view_resized",),
    "bitwig_vst3_setbusarrangements_while_active": ("bus_layout_proposal",),
    "wavelab_state_blob_fallback": ("deserialize_plugin_state",),
    "wavelab_vst3_defer_activation": ("bus_layout_proposal",),
    "logic_au_tail_time_conversion": ("prepare",),
}

CAPABILITY_RE = re.compile(r"^[a-z][a-z0-9_-]*$")
CAPABILITY_LOG_EVENTS: dict[str, tuple[str, ...]] = {
    "ara": ("begin_editing",),
    "load": ("session_start", "prepare"),
    "midi": ("midi_in",),
    "multi-bus": ("bus_layout_proposal",),
    "params": ("define_parameters", "serialize_plugin_state"),
    "sidechain": ("sidechain_edge",),
}
CAPABILITY_EVENT_MATCH_ALL = {"params"}


@dataclass(frozen=True)
class ValidationResult:
    path: pathlib.Path
    errors: tuple[str, ...]

    @property
    def ok(self) -> bool:
        return not self.errors


def _load_json(path: pathlib.Path) -> tuple[dict[str, Any] | None, list[str]]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        return None, [f"cannot read manifest: {exc}"]
    except json.JSONDecodeError as exc:
        return None, [f"invalid JSON: {exc.msg} at line {exc.lineno} column {exc.colno}"]
    if not isinstance(data, dict):
        return None, ["manifest root must be a JSON object"]
    return data, []


def _is_placeholder(value: str) -> bool:
    return bool(PLACEHOLDER_RE.search(value.strip()))


def _date_is_valid(value: str) -> bool:
    try:
        parsed = _dt.date.fromisoformat(value)
    except ValueError:
        return False
    return parsed.isoformat() == value


def _relative_existing_file(base: pathlib.Path, value: str, *, repo_root: pathlib.Path) -> pathlib.Path | None:
    candidate = pathlib.Path(value)
    if candidate.is_absolute():
        return candidate if candidate.is_file() else None

    local = (base / candidate).resolve()
    if local.is_file():
        return local

    repo = (repo_root / candidate).resolve()
    if repo.is_file():
        return repo

    return None


def _validate_quirks(data: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    quirks = data.get("quirks")
    if not isinstance(quirks, list) or not quirks:
        return ["quirks must be a non-empty array"]

    for index, quirk in enumerate(quirks):
        prefix = f"quirks[{index}]"
        if not isinstance(quirk, dict):
            errors.append(f"{prefix} must be an object")
            continue
        flag = quirk.get("flag")
        row = quirk.get("row")
        observed = quirk.get("observed")
        notes = quirk.get("notes")
        if not isinstance(flag, str) or not FLAG_RE.match(flag):
            errors.append(f"{prefix}.flag must be a quirk identifier")
        if not isinstance(row, str) or not row.strip() or _is_placeholder(row):
            errors.append(f"{prefix}.row must identify the script/catalog row")
        if observed not in ALLOWED_RESULTS:
            errors.append(
                f"{prefix}.observed must be one of: {', '.join(sorted(ALLOWED_RESULTS))}"
            )
        if not isinstance(notes, str) or not notes.strip() or _is_placeholder(notes):
            errors.append(f"{prefix}.notes must describe the observed evidence")
    return errors


def _validate_capabilities(data: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    capabilities = data.get("capabilities")
    if capabilities is None:
        return []
    if not isinstance(capabilities, list):
        return ["capabilities must be an array when present"]

    seen: set[str] = set()
    for index, capability in enumerate(capabilities):
        prefix = f"capabilities[{index}]"
        if not isinstance(capability, dict):
            errors.append(f"{prefix} must be an object")
            continue
        name = capability.get("capability")
        observed = capability.get("observed")
        notes = capability.get("notes")
        if not isinstance(name, str) or not CAPABILITY_RE.match(name):
            errors.append(f"{prefix}.capability must be a capability identifier")
        elif name in seen:
            errors.append(f"{prefix}.capability duplicates {name}")
        else:
            seen.add(name)
        if observed not in ALLOWED_RESULTS:
            errors.append(
                f"{prefix}.observed must be one of: {', '.join(sorted(ALLOWED_RESULTS))}"
            )
        if not isinstance(notes, str) or not notes.strip() or _is_placeholder(notes):
            errors.append(f"{prefix}.notes must describe the observed evidence")
    return errors


def _events_in_log(path: pathlib.Path) -> set[str]:
    events: set[str] = set()
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                match = LOG_LINE_RE.match(line)
                if match:
                    events.add(match.group("event"))
    except OSError:
        return events
    return events


def _plugin_versions_in_log(path: pathlib.Path) -> set[str]:
    versions: set[str] = set()
    try:
        with path.open("r", encoding="utf-8", errors="replace", newline="") as f:
            for row in csv.reader(f, delimiter="\t"):
                for field in row[2:]:
                    key, sep, value = field.partition("=")
                    if sep and key in LOG_PLUGIN_VERSION_KEYS and value.strip():
                        versions.add(value.strip())
    except OSError:
        return versions
    return versions


def _validate_log_plugin_version(
    data: dict[str, Any],
    log_paths: list[pathlib.Path],
) -> list[str]:
    manifest_version = data.get("plugin_version")
    if (
        not log_paths
        or not isinstance(manifest_version, str)
        or not manifest_version.strip()
        or _is_placeholder(manifest_version)
    ):
        return []

    observed_versions: set[str] = set()
    for path in log_paths:
        observed_versions.update(_plugin_versions_in_log(path))

    if not observed_versions or observed_versions == {manifest_version}:
        return []

    return [
        "plugin_version "
        f"{manifest_version} must match all checked-in log version(s): "
        f"{', '.join(sorted(observed_versions))}"
    ]


def _validate_claimed_log_events(
    data: dict[str, Any],
    log_paths: list[pathlib.Path],
) -> list[str]:
    if not log_paths:
        return []

    observed_events: set[str] = set()
    for path in log_paths:
        observed_events.update(_events_in_log(path))

    errors: list[str] = []
    quirks = data.get("quirks")
    if not isinstance(quirks, list):
        return errors

    for index, quirk in enumerate(quirks):
        if not isinstance(quirk, dict):
            continue
        flag = quirk.get("flag")
        observed = quirk.get("observed")
        if not isinstance(flag, str):
            continue
        expected_events = FLAG_LOG_EVENTS.get(flag)
        if not expected_events:
            continue
        found = sorted(set(expected_events).intersection(observed_events))
        expected = ", ".join(expected_events)
        if observed == "Confirmed" and not found:
            errors.append(
                f"quirks[{index}] {flag} is Confirmed but checked-in logs "
                f"do not contain expected event(s): {expected}"
            )
        elif observed == "Not Triggered" and found:
            errors.append(
                f"quirks[{index}] {flag} is Not Triggered but checked-in logs "
                f"contain expected event(s): {', '.join(found)}"
            )

    capabilities = data.get("capabilities")
    if not isinstance(capabilities, list):
        return errors

    for index, capability in enumerate(capabilities):
        if not isinstance(capability, dict):
            continue
        name = capability.get("capability")
        observed = capability.get("observed")
        if not isinstance(name, str):
            continue
        expected_events = CAPABILITY_LOG_EVENTS.get(name)
        if not expected_events:
            continue
        found = sorted(set(expected_events).intersection(observed_events))
        expected = ", ".join(expected_events)
        if observed == "Confirmed":
            if name in CAPABILITY_EVENT_MATCH_ALL:
                missing = [event for event in expected_events if event not in observed_events]
                if missing:
                    errors.append(
                        f"capabilities[{index}] {name} is Confirmed but checked-in logs "
                        f"do not contain required event(s): {', '.join(missing)}"
                    )
            elif not found:
                errors.append(
                    f"capabilities[{index}] {name} is Confirmed but checked-in logs "
                    f"do not contain expected event(s): {expected}"
                )
        elif observed == "Not Triggered" and found:
            errors.append(
                f"capabilities[{index}] {name} is Not Triggered but checked-in logs "
                f"contain expected event(s): {', '.join(found)}"
            )
    return errors


def _validate_preflight_report(path: pathlib.Path, prefix: str) -> list[str]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        return [f"{prefix} cannot be read: {exc}"]
    except json.JSONDecodeError as exc:
        return [f"{prefix} invalid JSON: {exc.msg} at line {exc.lineno} column {exc.colno}"]

    if not isinstance(data, dict):
        return [f"{prefix} root must be a JSON object"]

    errors: list[str] = []
    if not isinstance(data.get("ok"), bool):
        errors.append(f"{prefix}.ok must be a boolean")

    checks = data.get("checks")
    if not isinstance(checks, list) or not checks:
        errors.append(f"{prefix}.checks must be a non-empty array")
        return errors

    for index, check in enumerate(checks):
        check_prefix = f"{prefix}.checks[{index}]"
        if not isinstance(check, dict):
            errors.append(f"{check_prefix} must be an object")
            continue
        if not isinstance(check.get("label"), str) or not check["label"].strip():
            errors.append(f"{check_prefix}.label must be a non-empty string")
        if not isinstance(check.get("ok"), bool):
            errors.append(f"{check_prefix}.ok must be a boolean")
        if not isinstance(check.get("detail"), str):
            errors.append(f"{check_prefix}.detail must be a string")
    return errors


def validate_manifest(path: pathlib.Path, *, repo_root: pathlib.Path = REPO_ROOT) -> ValidationResult:
    data, errors = _load_json(path)
    if data is None:
        return ValidationResult(path, tuple(errors))

    base = path.parent

    if data.get("schema_version") != SCHEMA_VERSION:
        errors.append(f"schema_version must be {SCHEMA_VERSION}")

    for field in REQUIRED_STRING_FIELDS:
        value = data.get(field)
        if not isinstance(value, str) or not value.strip():
            errors.append(f"{field} must be a non-empty string")
        elif _is_placeholder(value):
            errors.append(f"{field} still contains a placeholder")

    fmt = data.get("format")
    if isinstance(fmt, str) and fmt not in ALLOWED_FORMATS:
        errors.append(f"format must be one of: {', '.join(sorted(ALLOWED_FORMATS))}")

    date = data.get("date")
    if isinstance(date, str) and not _date_is_valid(date):
        errors.append("date must be YYYY-MM-DD")
    elif isinstance(date, str):
        parent_date = base.name
        if _date_is_valid(parent_date) and parent_date != date:
            errors.append(f"date must match parent results folder ({parent_date})")

    commit = data.get("pulp_commit")
    if isinstance(commit, str) and not COMMIT_RE.match(commit):
        errors.append("pulp_commit must be a 7-40 character lowercase git hash")

    script = data.get("script")
    if isinstance(script, str) and not _is_placeholder(script):
        daw_bench_dir = (repo_root / "docs" / "validation" / "daw-bench").resolve()
        script_path = (daw_bench_dir / script).resolve()
        if not script_path.is_file() or not script_path.is_relative_to(daw_bench_dir):
            errors.append("script must reference a checked-in daw-bench script")

    result_markdown = data.get("result_markdown")
    if isinstance(result_markdown, str) and not _is_placeholder(result_markdown):
        if _relative_existing_file(base, result_markdown, repo_root=repo_root) is None:
            errors.append("result_markdown must reference a checked-in result markdown file")

    logs = data.get("logs", [])
    log_paths: list[pathlib.Path] = []
    external_log_url = data.get("external_log_url")
    if logs is None:
        logs = []
    if not isinstance(logs, list):
        errors.append("logs must be an array when present")
    else:
        for index, log in enumerate(logs):
            if not isinstance(log, str) or not log.strip() or _is_placeholder(log):
                errors.append(f"logs[{index}] must be a non-placeholder path")
            else:
                log_path = _relative_existing_file(base, log, repo_root=repo_root)
                if log_path is None:
                    errors.append(f"logs[{index}] must reference a checked-in log file")
                else:
                    log_paths.append(log_path)
    if external_log_url is not None:
        if not isinstance(external_log_url, str) or not external_log_url.startswith(("https://", "http://")):
            errors.append("external_log_url must be an http(s) URL when present")
    if logs == [] and external_log_url is None:
        errors.append("provide at least one checked-in log or external_log_url")

    preflight_reports = data.get("preflight_reports", [])
    if preflight_reports is None:
        preflight_reports = []
    if not isinstance(preflight_reports, list):
        errors.append("preflight_reports must be an array when present")
    else:
        for index, report in enumerate(preflight_reports):
            prefix = f"preflight_reports[{index}]"
            if not isinstance(report, str) or not report.strip() or _is_placeholder(report):
                errors.append(f"{prefix} must be a non-placeholder path")
                continue
            report_path = _relative_existing_file(base, report, repo_root=repo_root)
            if report_path is None:
                errors.append(f"{prefix} must reference a checked-in preflight JSON file")
                continue
            errors.extend(_validate_preflight_report(report_path, prefix))

    errors.extend(_validate_quirks(data))
    errors.extend(_validate_capabilities(data))
    errors.extend(_validate_log_plugin_version(data, log_paths))
    errors.extend(_validate_claimed_log_events(data, log_paths))
    return ValidationResult(path, tuple(errors))


def find_manifests(paths: list[pathlib.Path]) -> list[pathlib.Path]:
    out: list[pathlib.Path] = []
    for path in paths:
        if path.is_dir():
            out.extend(sorted(p for p in path.rglob(f"*{MANIFEST_SUFFIX}") if p.is_file()))
        elif path.name.endswith(MANIFEST_SUFFIX):
            out.append(path)
        else:
            out.append(path)
    return out


def render_results(results: list[ValidationResult], *, scanned: int) -> str:
    if not results:
        return f"daw-bench evidence: no manifests found in {scanned} path(s)."

    lines: list[str] = []
    for result in results:
        rel = result.path
        try:
            rel = result.path.relative_to(REPO_ROOT)
        except ValueError:
            pass
        if result.ok:
            lines.append(f"OK {rel}")
        else:
            lines.append(f"FAIL {rel}")
            for error in result.errors:
                lines.append(f"  - {error}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=pathlib.Path, default=[DEFAULT_RESULTS_DIR])
    parser.add_argument("--require-any", action="store_true",
                        help="fail if no manifests are found")
    args = parser.parse_args(argv)

    manifests = find_manifests(args.paths)
    if not manifests:
        print(render_results([], scanned=len(args.paths)))
        return 1 if args.require_any else 0

    results = [validate_manifest(path) for path in manifests]
    print(render_results(results, scanned=len(args.paths)))
    return 0 if all(result.ok for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
