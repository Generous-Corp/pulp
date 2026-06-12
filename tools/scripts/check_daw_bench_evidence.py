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
    external_log_url = data.get("external_log_url")
    if logs is None:
        logs = []
    if not isinstance(logs, list):
        errors.append("logs must be an array when present")
    else:
        for index, log in enumerate(logs):
            if not isinstance(log, str) or not log.strip() or _is_placeholder(log):
                errors.append(f"logs[{index}] must be a non-placeholder path")
            elif _relative_existing_file(base, log, repo_root=repo_root) is None:
                errors.append(f"logs[{index}] must reference a checked-in log file")
    if external_log_url is not None:
        if not isinstance(external_log_url, str) or not external_log_url.startswith(("https://", "http://")):
            errors.append("external_log_url must be an http(s) URL when present")
    if logs == [] and external_log_url is None:
        errors.append("provide at least one checked-in log or external_log_url")

    errors.extend(_validate_quirks(data))
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
