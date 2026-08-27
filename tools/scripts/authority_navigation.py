#!/usr/bin/env python3
"""Validate Pulp's routing-only authority navigation registry."""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from typing import Any

import json_schema_lite


ROOT = pathlib.Path(__file__).resolve().parents[2]
REGISTRY = ROOT / "docs/status/authority-navigation.json"
SCHEMA = ROOT / "docs/status/authority-navigation.schema.json"
TOKEN_RE = re.compile(r"^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$")
ABSENCE_VALUES = {
    "not_advertised_by_the_queried_mcp_server",
    "not_present_in_covered_forge_bake_catalog_headers",
    "not_present_in_joined_forge_catalog",
    "not_registered_as_a_top_level_cli_command",
    "not_registered_in_builtin_timeline_schema_registry",
    "requires_exact_live_instance",
    "unknown",
    "unknown_outside_reverified_rows",
}
EXPECTED_IDS = {
    "agent-capabilities",
    "dsp-capabilities",
    "dsp-survey-admission",
    "forge-catalog",
    "live-control",
    "offline-cli",
    "offline-mcp",
    "sequencer-exposure",
    "timeline-schema",
}
EXACT_LIVE_QUERY = (
    "pulp control status --instance <caller-supplied-exact-instance-id> --json"
)
DSP_ADMISSION_OWNER = (
    "danielraffel/pulp-planning:dsp-survey-claims/WORKER-PROMPT.md"
)
DSP_ADMISSION_QUERY = (
    "read dsp-survey-claims/WORKER-PROMPT.md from current "
    "danielraffel/pulp-planning main"
)


def _safe_relative(value: str, *, installed: bool) -> bool:
    if not value or "\\" in value:
        return False
    path = pathlib.PurePosixPath(value)
    if path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        return False
    if installed and path.parts[0] not in {"bin", "share"}:
        return False
    if installed and path.parts[0] == "share" and path.parts[:2] != ("share", "pulp"):
        return False
    return True


def validate_document(
    document: Any,
    schema: Any,
    repo_root: pathlib.Path,
    *,
    require_source_locations: bool = True,
) -> list[str]:
    problems = list(json_schema_lite.validate(document, schema))
    if not isinstance(document, dict):
        return problems or ["registry root must be an object"]

    rows = document.get("authorities")
    if not isinstance(rows, list):
        return problems or ["authorities must be an array"]

    ids = [row.get("id") for row in rows if isinstance(row, dict)]
    if ids != sorted(ids):
        problems.append("authority rows must be bytewise sorted by id")
    if set(ids) != EXPECTED_IDS or len(ids) != len(EXPECTED_IDS):
        problems.append("authority rows must contain exactly the finite v1 authority ids")

    claimed: dict[str, str] = {}
    for index, row in enumerate(rows):
        prefix = f"authorities[{index}]"
        if not isinstance(row, dict):
            continue
        authority_id = row.get("id")
        if not isinstance(authority_id, str) or not TOKEN_RE.fullmatch(authority_id):
            continue

        aliases = row.get("aliases")
        if isinstance(aliases, list):
            if aliases != sorted(aliases):
                problems.append(f"{prefix}.aliases must be bytewise sorted")
            for token in [authority_id, *aliases]:
                if not isinstance(token, str) or not TOKEN_RE.fullmatch(token):
                    continue
                if token == authority_id and token in aliases:
                    problems.append(f"{prefix}.aliases repeats its canonical id {token!r}")
                prior = claimed.get(token)
                if prior is not None:
                    problems.append(
                        f"authority token {token!r} is claimed by both {prior!r} and {authority_id!r}"
                    )
                else:
                    claimed[token] = authority_id

        source = row.get("source_location")
        if isinstance(source, str):
            if not _safe_relative(source, installed=False):
                problems.append(f"{prefix}.source_location is not a safe repository-relative path")
            elif require_source_locations and not (
                repo_root / pathlib.PurePosixPath(source)
            ).exists():
                problems.append(f"{prefix}.source_location does not exist: {source}")

        installed = row.get("installed_location")
        if isinstance(installed, str) and not _safe_relative(installed, installed=True):
            problems.append(f"{prefix}.installed_location is not a safe prefix-relative path")

        query = row.get("query_or_validator")
        if isinstance(query, dict):
            source_query = query.get("source")
            installed_query = query.get("installed")
            for context, value in (("source", source_query), ("installed", installed_query)):
                if isinstance(value, str) and "pulp authority" in value:
                    problems.append(
                        f"{prefix}.query_or_validator.{context} must route to a native authority"
                    )
            if installed is not None and not isinstance(installed_query, str):
                problems.append(
                    f"{prefix} has an installed artifact but no installed native query"
                )

        absence = row.get("absence_semantics")
        if absence not in ABSENCE_VALUES:
            problems.append(f"{prefix}.absence_semantics is not a supported v1 value")

        if row.get("plane") == "live_control":
            route = query.get("installed") if isinstance(query, dict) else None
            if absence != "requires_exact_live_instance":
                problems.append("live_control must use requires_exact_live_instance")
            source_route = query.get("source") if isinstance(query, dict) else None
            if route != EXACT_LIVE_QUERY or source_route != EXACT_LIVE_QUERY:
                problems.append(
                    "live_control route must equal the exact caller-supplied-instance query"
                )

        if authority_id == "dsp-survey-admission":
            if (
                row.get("plane") != "release_evidence"
                or row.get("native_owner") != DSP_ADMISSION_OWNER
                or row.get("source_location") != "planning"
                or installed is not None
                or not isinstance(query, dict)
                or query.get("source") != DSP_ADMISSION_QUERY
                or query.get("installed") is not None
            ):
                problems.append(
                    "dsp-survey-admission must remain source-only and route to current pulp-planning main"
                )

        for field in ("native_owner", "coverage_semantics", "absence_semantics"):
            value = row.get(field)
            if isinstance(value, str) and value != value.strip():
                problems.append(f"{prefix}.{field} must be trimmed")
        disclaimers = row.get("does_not_prove")
        if isinstance(disclaimers, list):
            for disclaimer in disclaimers:
                if isinstance(disclaimer, str) and disclaimer != disclaimer.strip():
                    problems.append(f"{prefix}.does_not_prove entries must be trimmed")

    return problems


def load_and_validate(
    registry_path: pathlib.Path = REGISTRY,
    schema_path: pathlib.Path = SCHEMA,
    repo_root: pathlib.Path = ROOT,
) -> dict[str, Any]:
    document = json.loads(registry_path.read_text(encoding="utf-8"))
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    problems = validate_document(document, schema, repo_root)
    if problems:
        raise ValueError("\n".join(problems))
    return document


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="validate the registry")
    parser.add_argument("--registry", type=pathlib.Path, default=REGISTRY)
    parser.add_argument("--schema", type=pathlib.Path, default=SCHEMA)
    parser.add_argument("--repo-root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args(argv)
    try:
        document = load_and_validate(args.registry, args.schema, args.repo_root)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"authority-navigation: INVALID: {exc}", file=sys.stderr)
        return 1
    print(
        "authority-navigation: OK: "
        f"revision {document['registry_revision']}, {len(document['authorities'])} routes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
