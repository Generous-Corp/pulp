#!/usr/bin/env python3
"""Validate host-matrix rows against checked-in DAW-bench evidence."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from dataclasses import dataclass
from typing import Any

import check_daw_bench_evidence as daw_bench


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
DEFAULT_MATRIX = REPO_ROOT / "docs" / "guides" / "host-matrix.md"
PROMOTED_STATUSES = {"✅", "🟡", "🔴"}
UNVALIDATED_STATUSES = {"—", "n/a", ""}
EVIDENCE_RE = re.compile(r"docs/validation/daw-bench/results/\d{4}-\d{2}-\d{2}/?")


@dataclass(frozen=True)
class MatrixRow:
    line: int
    format: str
    host: str
    capabilities: dict[str, str]
    notes: str

    @property
    def promoted(self) -> bool:
        return any(status in PROMOTED_STATUSES for status in self.capabilities.values())


@dataclass(frozen=True)
class MatrixCheck:
    path: pathlib.Path
    errors: tuple[str, ...]

    @property
    def ok(self) -> bool:
        return not self.errors


def _split_row(line: str) -> list[str]:
    return [part.strip() for part in line.strip().strip("|").split("|")]


def _format_heading(line: str) -> str | None:
    match = re.match(r"^##\s+(.+?)\s*$", line)
    if not match:
        return None
    heading = match.group(1).strip()
    return heading.split()[0]


def parse_matrix(path: pathlib.Path = DEFAULT_MATRIX) -> list[MatrixRow]:
    rows: list[MatrixRow] = []
    current_format: str | None = None
    headers: list[str] = []
    in_table = False

    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        heading = _format_heading(line)
        if heading:
            current_format = heading
            headers = []
            in_table = False
            continue

        if not current_format:
            continue
        if line.startswith("| Host"):
            headers = _split_row(line)
            in_table = True
            continue
        if in_table and re.match(r"^\|[-:| ]+\|?$", line):
            continue
        if not in_table or not line.startswith("|"):
            if in_table and line.strip() == "":
                in_table = False
            continue

        cells = _split_row(line)
        if len(cells) != len(headers) or not cells:
            continue
        row = dict(zip(headers, cells))
        host = row.get("Host", "")
        notes = row.get("Notes", "")
        capabilities = {
            key: value
            for key, value in row.items()
            if key not in {"Host", "Version", "Notes"} and value not in UNVALIDATED_STATUSES
        }
        rows.append(MatrixRow(
            line=lineno,
            format=current_format,
            host=host,
            capabilities=capabilities,
            notes=notes,
        ))

    return rows


def _normalize(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", value.casefold())


def _manifest_matches_lane(path: pathlib.Path, *, host: str, fmt: str) -> bool:
    try:
        data: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    return _normalize(str(data.get("host", ""))) == _normalize(host) and (
        _normalize(str(data.get("format", ""))) == _normalize(fmt)
    )


def _manifest_observes_capability(path: pathlib.Path, capability: str, observed: str) -> bool:
    try:
        data: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    capabilities = data.get("capabilities")
    if not isinstance(capabilities, list):
        return False
    expected = _normalize(capability)
    for item in capabilities:
        if not isinstance(item, dict):
            continue
        name = item.get("capability")
        actual = item.get("observed")
        if isinstance(name, str) and _normalize(name) == expected:
            return actual == observed
    return False


def validate_matrix(
    matrix_path: pathlib.Path = DEFAULT_MATRIX,
    *,
    repo_root: pathlib.Path = REPO_ROOT,
) -> MatrixCheck:
    errors: list[str] = []
    rows = parse_matrix(matrix_path)

    for row in rows:
        if not row.promoted:
            continue
        citations = EVIDENCE_RE.findall(row.notes)
        if not citations:
            errors.append(
                f"{matrix_path}:{row.line} {row.format}/{row.host} has promoted "
                "status but no DAW-bench result folder in Notes"
            )
            continue

        matching_manifests: list[pathlib.Path] = []
        for citation in citations:
            result_dir = (repo_root / citation).resolve()
            if not result_dir.is_dir():
                errors.append(
                    f"{matrix_path}:{row.line} cited DAW-bench folder does not exist: {citation}"
                )
                continue
            manifests = daw_bench.find_manifests([result_dir])
            if not manifests:
                errors.append(
                    f"{matrix_path}:{row.line} cited DAW-bench folder has no manifests: {citation}"
                )
                continue
            results = [daw_bench.validate_manifest(path, repo_root=repo_root) for path in manifests]
            for result in results:
                if not result.ok:
                    rendered = ", ".join(result.errors)
                    errors.append(f"{matrix_path}:{row.line} invalid evidence {result.path}: {rendered}")
            if any(not result.ok for result in results):
                continue
            matching_manifests.extend(
                path for path in manifests
                if _manifest_matches_lane(path, host=row.host, fmt=row.format)
            )

        if not matching_manifests:
            errors.append(
                f"{matrix_path}:{row.line} no valid DAW-bench manifest matches "
                f"{row.format}/{row.host}"
            )
            continue

        for capability, status in row.capabilities.items():
            if status not in PROMOTED_STATUSES:
                continue
            required_observed = "Refuted" if status == "🔴" else "Confirmed"
            if not any(
                _manifest_observes_capability(path, capability, required_observed)
                for path in matching_manifests
            ):
                errors.append(
                    f"{matrix_path}:{row.line} no valid DAW-bench manifest confirms "
                    f"{row.format}/{row.host} capability {capability} as {required_observed}"
                )

    return MatrixCheck(matrix_path, tuple(errors))


def render_check(check: MatrixCheck, *, repo_root: pathlib.Path = REPO_ROOT) -> str:
    try:
        rel = check.path.resolve().relative_to(repo_root.resolve())
    except ValueError:
        rel = check.path
    if check.ok:
        return f"OK {rel}"
    lines = [f"FAIL {rel}"]
    lines.extend(f"  - {error}" for error in check.errors)
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("matrix", nargs="?", type=pathlib.Path, default=DEFAULT_MATRIX)
    args = parser.parse_args(argv)

    check = validate_matrix(args.matrix)
    print(render_check(check))
    return 0 if check.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
