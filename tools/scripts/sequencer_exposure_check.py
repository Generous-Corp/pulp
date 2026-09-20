#!/usr/bin/env python3
"""Fail-closed validation for the sequencer cross-surface exposure ledger."""

from __future__ import annotations

import argparse
import dataclasses
import json
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Any

from gate_common import (
    GitComparisonProvenance,
    git_comparison_receipt,
    read_git_path,
    resolve_git_comparison,
)


CLASSIFICATIONS = {
    "ci_test_only",
    "engine_internal",
    "public_sdk",
    "offline_cli_mcp",
    "live_product_control",
}
SURFACES = {
    "installed_sdk",
    "offline_timeline_cli",
    "offline_timeline_mcp",
    "live_product_control",
    "design_time_agent_manifest",
}
DISPOSITIONS = {"exposed", "gap", "not_applicable", "deferred"}
DELIVERY_STATES = {"pending", "released"}
EVIDENCE_KINDS = {
    "sdk_header",
    "sdk_target",
    "cli_definition",
    "cli_handler",
    "mcp_definition",
    "mcp_handler",
    "capability_definition",
    "operation_definition",
    "executor_binding",
    "profile_policy",
    "cli_projection",
    "mcp_projection",
    "test",
    "agent_manifest",
    "doc",
    "skill",
    "other",
}
REQUIRED_LIVE_KINDS = {
    "capability_definition",
    "operation_definition",
    "executor_binding",
    "profile_policy",
    "cli_projection",
    "mcp_projection",
    "test",
}
ADMISSIONS = {
    "in_process",
    "registered_writer",
    "read_only",
    "descriptor",
}
REQUIRED_AUTHORITY_KEYS = {
    "admission",
    "writer_profile",
    "bounds",
    "refusal_codes",
}
REQUIRED_BOUNDS_KEYS = {
    "max_transaction_retained_bytes",
    "max_session_retained_bytes",
}
# The authority vocabulary has one source and two readers. The offline writer
# profiles are defined in C++ for the CLI and MCP boundaries; this gate parses
# that same source rather than transcribing it, so a ledger row cannot describe
# a profile, quota or refusal code the boundaries do not actually implement.
WRITER_PROFILE_HEADER = "tools/timeline/include/pulp/tools/timeline/writer_profile.hpp"
WRITER_PROFILE_SOURCE = "tools/timeline/src/writer_profile.cpp"

SHA_RE = re.compile(r"^[0-9a-f]{40}$")
ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
# The ledger has two on-disk forms and the checker reads either, or both at
# once. The single document is the original form; the directory is one file per
# row. A single document has one append point, so two branches that each add a
# row rewrite the same bytes and conflict *with each other* even when each
# merges cleanly against main — which also makes the merge queue refuse to batch
# them. One file per row gives two such branches disjoint paths. Both forms are
# read during the migration so moving the already-merged rows is a separate,
# mechanical change that does not have to happen on the same day.
LEDGER_PATH = "docs/status/sequencer-exposure.json"
LEDGER_DIR = "docs/status/sequencer-exposure"
LEDGER_HEADER_PATH = f"{LEDGER_DIR}/ledger.json"
LEDGER_ROWS_DIR = f"{LEDGER_DIR}/rows"
LEDGER_TOMBSTONES_DIR = f"{LEDGER_DIR}/tombstones"
HEADER_KEYS = ("schema_version", "ledger_id", "audit")
E0_INFRASTRUCTURE_PATHS = {
    LEDGER_PATH,
    "tools/scripts/sequencer_exposure_check.py",
    "tools/scripts/test_sequencer_exposure_check.py",
}
# Files a repo tool rewrites wholesale. The gpu-handoff pin-freshness gate
# *requires* the refresh whenever a pinned path changes, so watching one of
# these whole would leave the author no way to satisfy both gates at once:
# this gate would reject the refreshed file for lacking a pending row. They are
# excluded from the whole-file watch below; sequencer semantics newly added to
# one of them is still governed by the semantic check.
GENERATED_ARTIFACT_PATHS = {
    "docs/status/gpu-vellum-handoff.yaml",
    "docs/validation/gpu-handoff-provenance/receipt.json",
    "tools/agent-capabilities/contract-history.json",
}
# Files the version bot rewrites wholesale on every bump, mapped to the keys it
# may rewrite. Excluding one WHOLE would drop real coverage, because the
# semantic scan below only reaches core/state, core/view, core/midi and
# inspect -- a docs/ artifact has no semantic backstop. So the exemption is
# scoped to the transition instead of the path: a diff confined to these keys
# with version-shaped values is mechanical and needs no ledger row, while any
# other edit to the same file stays watched and still demands one.
MECHANICAL_VERSION_ARTIFACTS: dict[str, frozenset[str]] = {
    "docs/status/pulp-tooling-disposition.json": frozenset(
        {"version", "catalog_version", "catalog_metadata_version", "min_cli_version"}
    ),
}
_VERSION_VALUE_RE = re.compile(r"^\d+(?:\.\d+)*(?:[-+][0-9A-Za-z.\-]+)?$")
_VERSION_LINE_RE = re.compile(r'^"([A-Za-z0-9_]+)"\s*:\s*"([^"]*)"$')


def _is_nonempty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _eval_byte_literal(expression: str) -> int | None:
    """Evaluate a C++ integer-constant expression such as ``1024ull * 1024ull``.

    Returns None for anything that is not a plain product/sum of literals, so a
    constant this gate cannot read fails closed rather than being guessed at.
    """
    cleaned = re.sub(r"\b(\d+)(?:ull|ul|ll|u|l)\b", r"\1", expression, flags=re.IGNORECASE)
    cleaned = cleaned.strip().rstrip(";").strip()
    if not cleaned or not re.fullmatch(r"[0-9+*()\s]+", cleaned):
        return None
    try:
        value = eval(cleaned, {"__builtins__": {}}, {})  # noqa: S307 - literal arithmetic only
    except (SyntaxError, ValueError, TypeError, ZeroDivisionError):
        return None
    return value if isinstance(value, int) else None


def _load_writer_profile_vocabulary(repo_root: Path) -> tuple[dict[str, dict[str, int | None]],
                                                              set[str],
                                                              list[str]]:
    """Read the offline writer authority vocabulary from its C++ definition.

    The ledger and the CLI/MCP boundaries must name the same profiles, quotas and
    refusal codes. Parsing the shipped source keeps that a single source of truth
    rather than a transcription that can drift silently. A source this cannot read
    is an error, never a skip: a gate that quietly stops checking is worse than no
    gate.
    """
    errors: list[str] = []
    header_path = repo_root / WRITER_PROFILE_HEADER
    source_path = repo_root / WRITER_PROFILE_SOURCE
    for path, label in ((header_path, WRITER_PROFILE_HEADER), (source_path, WRITER_PROFILE_SOURCE)):
        if not path.is_file():
            errors.append(f"authority vocabulary: {label} is missing; cannot validate authority")
    if errors:
        return {}, set(), errors
    header = header_path.read_text(encoding="utf-8")
    source = source_path.read_text(encoding="utf-8")

    by_name = re.search(
        r"writer_profile_by_name\b.*?\{(.*?)\n\}", source, re.S
    )
    if by_name is None:
        return {}, set(), ["authority vocabulary: cannot locate writer_profile_by_name"]
    names = re.findall(r'name\s*==\s*"([a-z_]+)"', by_name.group(1))
    if not names:
        return {}, set(), ["authority vocabulary: writer_profile_by_name names no profiles"]

    profiles: dict[str, dict[str, int | None]] = {}
    for name in names:
        camel = name.capitalize()
        bounds: dict[str, int | None] = {}
        for ledger_key, cpp_key in (
            ("max_transaction_retained_bytes", f"k{camel}MaxTransactionRetainedBytes"),
            ("max_session_retained_bytes", f"k{camel}MaxSessionRetainedBytes"),
        ):
            match = re.search(rf"\b{cpp_key}\s*=\s*([^;]+);", header)
            if match is None:
                # No constant means the profile is deliberately unquotaed, which
                # the boundaries publish as a null ceiling.
                bounds[ledger_key] = None
                continue
            value = _eval_byte_literal(match.group(1))
            if value is None:
                errors.append(
                    f"authority vocabulary: cannot evaluate {cpp_key} in {WRITER_PROFILE_HEADER}"
                )
                bounds[ledger_key] = None
                continue
            bounds[ledger_key] = value
        profiles[name] = bounds

    codes_body = re.search(r"conflict_code_name\b.*?\{(.*?)\n\}", source, re.S)
    if codes_body is None:
        return profiles, set(), errors + ["authority vocabulary: cannot locate conflict_code_name"]
    codes = set(re.findall(r'return\s+"([a-z_]+)";', codes_body.group(1)))
    if not codes:
        errors.append("authority vocabulary: conflict_code_name names no refusal codes")
    return profiles, codes, errors


def _exact_keys(value: Any, required: set[str], optional: set[str], where: str,
                errors: list[str]) -> bool:
    if not isinstance(value, dict):
        errors.append(f"{where}: expected object")
        return False
    keys = set(value)
    missing = required - keys
    extra = keys - required - optional
    if missing:
        errors.append(f"{where}: missing fields: {', '.join(sorted(missing))}")
    if extra:
        errors.append(f"{where}: unknown fields: {', '.join(sorted(extra))}")
    return not missing and not extra


def _safe_repo_path(repo_root: Path, raw: Any, where: str, errors: list[str]) -> Path | None:
    if not _is_nonempty_string(raw):
        errors.append(f"{where}: path must be a nonempty string")
        return None
    pure = PurePosixPath(raw)
    if pure.is_absolute() or ".." in pure.parts or "\\" in raw:
        errors.append(f"{where}: path must be a repository-relative POSIX path: {raw!r}")
        return None
    candidate = repo_root.joinpath(*pure.parts)
    try:
        candidate.resolve(strict=False).relative_to(repo_root.resolve())
    except ValueError:
        errors.append(f"{where}: path escapes repository: {raw!r}")
        return None
    return candidate


def _validate_evidence(
    item: Any,
    repo_root: Path,
    where: str,
    errors: list[str],
    *,
    require_kind: bool = True,
    validate_current_path: bool = True,
) -> str | None:
    required = {"path", "needles"} | ({"kind"} if require_kind else set())
    if not _exact_keys(item, required, set(), where, errors):
        return None
    kind = item.get("kind") if require_kind else None
    if require_kind and kind not in EVIDENCE_KINDS:
        errors.append(f"{where}.kind: unknown evidence kind {kind!r}")
    path = _safe_repo_path(repo_root, item.get("path"), f"{where}.path", errors)
    needles = item.get("needles")
    if (not isinstance(needles, list) or not needles or
            any(not _is_nonempty_string(needle) for needle in needles)):
        errors.append(f"{where}.needles: expected a nonempty list of nonempty strings")
        needles = []
    elif len(needles) != len(set(needles)):
        errors.append(f"{where}.needles: duplicate needle")
    if path is not None and validate_current_path:
        if not path.is_file():
            errors.append(f"{where}.path: evidence file does not exist: {item['path']}")
        else:
            try:
                contents = path.read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError) as error:
                errors.append(f"{where}.path: cannot read text evidence: {error}")
            else:
                for needle in needles:
                    if needle not in contents:
                        errors.append(
                            f"{where}: stale evidence; {needle!r} not found in {item['path']}"
                        )
    return kind if require_kind and kind in EVIDENCE_KINDS else None


def _validate_authority(
    authority: Any,
    disposition: Any,
    surface_name: str,
    where: str,
    errors: list[str],
    profiles: dict[str, dict[str, int | None]],
    refusal_codes: set[str],
) -> None:
    """Validate the authority an exposed surface admits a caller under.

    An exposed surface has to state which authority it grants, because a reader
    cannot otherwise tell a quota-bounded proposal writer from an unrestricted
    one. A surface that grants nothing states that explicitly rather than by
    omission.
    """
    if disposition != "exposed":
        if authority is not None:
            errors.append(
                f"{where}.authority: only an exposed surface carries an authority descriptor"
            )
        return
    if authority is None:
        errors.append(f"{where}.authority: required for an exposed surface")
        return
    if not _exact_keys(authority, REQUIRED_AUTHORITY_KEYS, set(), f"{where}.authority", errors):
        return

    admission = authority.get("admission")
    if admission not in ADMISSIONS:
        errors.append(f"{where}.authority.admission: unknown admission {admission!r}")
        return

    profile = authority.get("writer_profile")
    bounds = authority.get("bounds")
    if admission == "registered_writer":
        if profile not in profiles:
            errors.append(
                f"{where}.authority.writer_profile: {profile!r} is not a profile "
                f"{WRITER_PROFILE_SOURCE} defines"
            )
        elif not isinstance(bounds, dict):
            errors.append(
                f"{where}.authority.bounds: a registered writer must state its retained-byte "
                "ceilings"
            )
        elif _exact_keys(bounds, REQUIRED_BOUNDS_KEYS, set(), f"{where}.authority.bounds", errors):
            expected = profiles[profile]
            for key in sorted(REQUIRED_BOUNDS_KEYS):
                if bounds.get(key) != expected[key]:
                    errors.append(
                        f"{where}.authority.bounds.{key}: {bounds.get(key)!r} disagrees with the "
                        f"{profile!r} profile in {WRITER_PROFILE_HEADER} ({expected[key]!r})"
                    )
    else:
        if profile is not None:
            errors.append(
                f"{where}.authority.writer_profile: must be null when admission is {admission!r}; "
                "no writer is registered"
            )
        if bounds is not None:
            errors.append(
                f"{where}.authority.bounds: must be null when admission is {admission!r}"
            )

    codes = authority.get("refusal_codes")
    if not isinstance(codes, list) or any(not _is_nonempty_string(code) for code in codes):
        errors.append(f"{where}.authority.refusal_codes: expected a list of strings")
        return
    if len(codes) != len(set(codes)):
        errors.append(f"{where}.authority.refusal_codes: duplicate refusal code")
    unknown = sorted(set(codes) - refusal_codes)
    if unknown:
        errors.append(
            f"{where}.authority.refusal_codes: {', '.join(unknown)} not named by "
            f"conflict_code_name in {WRITER_PROFILE_SOURCE}"
        )
    if admission == "registered_writer" and not codes:
        errors.append(
            f"{where}.authority.refusal_codes: a registered writer must name the refusals it emits"
        )
    if admission != "registered_writer" and codes:
        errors.append(
            f"{where}.authority.refusal_codes: must be empty when admission is {admission!r}"
        )


def _validate_surface(
    surface: Any,
    name: str,
    repo_root: Path,
    where: str,
    errors: list[str],
    *,
    validate_current_paths: bool,
    profiles: dict[str, dict[str, int | None]],
    refusal_codes: set[str],
) -> set[str]:
    if not _exact_keys(
        surface,
        {"disposition", "rationale"},
        {"owner", "dependencies", "evidence", "authority"},
        where,
        errors,
    ):
        return set()
    disposition = surface.get("disposition")
    _validate_authority(
        surface.get("authority"), disposition, name, where, errors, profiles, refusal_codes
    )
    if disposition not in DISPOSITIONS:
        errors.append(f"{where}.disposition: unknown disposition {disposition!r}")
    if not _is_nonempty_string(surface.get("rationale")):
        errors.append(f"{where}.rationale: must be nonempty")
    dependencies = surface.get("dependencies", [])
    if (not isinstance(dependencies, list) or
            any(not _is_nonempty_string(item) or not ID_RE.fullmatch(item)
                for item in dependencies)):
        errors.append(f"{where}.dependencies: expected unique identifier strings")
    elif len(dependencies) != len(set(dependencies)):
        errors.append(f"{where}.dependencies: duplicate dependency")
    if disposition in {"gap", "deferred"} and not _is_nonempty_string(surface.get("owner")):
        errors.append(f"{where}.owner: required for {disposition}")
    if disposition in {"gap", "deferred"} and isinstance(dependencies, list) and not dependencies:
        errors.append(f"{where}.dependencies: required for {disposition}")
    evidence = surface.get("evidence", [])
    if not isinstance(evidence, list):
        errors.append(f"{where}.evidence: expected array")
        return set()
    if disposition == "exposed" and not evidence:
        errors.append(f"{where}.evidence: exposed surface requires evidence")
    kinds = {
        kind
        for index, item in enumerate(evidence)
        if (
            kind := _validate_evidence(
                item,
                repo_root,
                f"{where}.evidence[{index}]",
                errors,
                validate_current_path=validate_current_paths,
            )
        )
    }
    if disposition == "exposed" and name == "installed_sdk":
        if not ({"sdk_header", "sdk_target"} & kinds):
            errors.append(f"{where}: installed SDK claim needs sdk_header or sdk_target evidence")
    if disposition == "exposed" and name == "offline_timeline_cli":
        missing = {"cli_definition", "cli_handler"} - kinds
        if missing:
            errors.append(f"{where}: offline CLI claim missing {', '.join(sorted(missing))}")
        for item in evidence:
            if item.get("kind") == "cli_handler":
                path = item.get("path", "")
                if not path.startswith("tools/") or path.startswith("core/timeline/schema/"):
                    errors.append(f"{where}: CLI handler must name real tools source, not generated metadata")
    if disposition == "exposed" and name == "offline_timeline_mcp":
        missing = {"mcp_definition", "mcp_handler"} - kinds
        if missing:
            errors.append(f"{where}: offline MCP claim missing {', '.join(sorted(missing))}")
        for item in evidence:
            if item.get("kind") == "mcp_handler":
                path = item.get("path", "")
                if not path.startswith("tools/mcp/") or not path.endswith((".cpp", ".hpp")):
                    errors.append(f"{where}: MCP handler must name a tools/mcp C++ source")
    if disposition == "exposed" and name == "live_product_control":
        missing = REQUIRED_LIVE_KINDS - kinds
        if missing:
            errors.append(f"{where}: live control claim missing {', '.join(sorted(missing))}")
        canonical_prefixes = {
            "capability_definition": ("inspect/include/pulp/inspect/capability_definitions.inc",),
            "operation_definition": ("inspect/src/control_manifest.cpp",),
            "executor_binding": ("inspect/src/", "inspect/include/"),
            "profile_policy": ("inspect/",),
            "cli_projection": ("tools/cli/",),
            "mcp_projection": ("tools/mcp/",),
            "test": ("test/", "inspect/test", "tools/"),
        }
        for item in evidence:
            kind = item.get("kind")
            if kind in canonical_prefixes:
                path = item.get("path", "")
                if not any(path.startswith(prefix) for prefix in canonical_prefixes[kind]):
                    errors.append(f"{where}: {kind} does not name a canonical Product-A source")
    return kinds


def validate_document(document: Any, repo_root: Path) -> list[str]:
    """Return every ledger error. An empty list means the document is valid."""
    errors: list[str] = []
    writer_profiles, writer_refusal_codes, vocabulary_errors = _load_writer_profile_vocabulary(
        repo_root
    )
    errors.extend(vocabulary_errors)
    if not _exact_keys(
        document, {"schema_version", "ledger_id", "audit", "rows", "tombstones"}, set(),
        "ledger", errors
    ):
        return errors
    if document.get("schema_version") != 2:
        errors.append("ledger.schema_version: expected 2")
    if document.get("ledger_id") != "dev.pulp.sequencer-exposure@2":
        errors.append("ledger.ledger_id: expected dev.pulp.sequencer-exposure@2")
    audit = document.get("audit")
    if _exact_keys(
        audit, {"status", "scope", "owner", "dependencies", "gaps"}, set(),
        "ledger.audit", errors
    ):
        if audit.get("status") not in {"in_progress", "complete"}:
            errors.append("ledger.audit.status: expected in_progress or complete")
        for field in ("scope", "owner"):
            if not _is_nonempty_string(audit.get(field)):
                errors.append(f"ledger.audit.{field}: must be nonempty")
        audit_dependencies = audit.get("dependencies")
        if (not isinstance(audit_dependencies, list) or
                any(not _is_nonempty_string(item) or not ID_RE.fullmatch(item)
                    for item in audit_dependencies) or
                (isinstance(audit_dependencies, list) and
                 len(audit_dependencies) != len(set(audit_dependencies)))):
            errors.append("ledger.audit.dependencies: expected unique identifier strings")
        gaps = audit.get("gaps")
        if not isinstance(gaps, list):
            errors.append("ledger.audit.gaps: expected array")
            gaps = []
        if audit.get("status") == "in_progress" and not gaps:
            errors.append("ledger.audit.gaps: in_progress audit requires a visible gap")
        if audit.get("status") == "complete" and gaps:
            errors.append("ledger.audit.gaps: complete audit cannot retain gaps")
        gap_ids: list[str] = []
        audit_surfaces = SURFACES | {"historical_census"}
        for gap_index, gap in enumerate(gaps):
            gap_where = f"ledger.audit.gaps[{gap_index}]"
            if not _exact_keys(
                gap, {"id", "surface", "owner", "dependencies", "rationale"},
                set(), gap_where, errors
            ):
                continue
            gap_id = gap.get("id")
            if not _is_nonempty_string(gap_id) or not ID_RE.fullmatch(gap_id):
                errors.append(f"{gap_where}.id: invalid identifier")
            else:
                gap_ids.append(gap_id)
            if gap.get("surface") not in audit_surfaces:
                errors.append(f"{gap_where}.surface: invalid audit surface")
            for field in ("owner", "rationale"):
                if not _is_nonempty_string(gap.get(field)):
                    errors.append(f"{gap_where}.{field}: must be nonempty")
            dependencies = gap.get("dependencies")
            if (not isinstance(dependencies, list) or not dependencies or
                    any(not _is_nonempty_string(item) or not ID_RE.fullmatch(item)
                        for item in dependencies) or
                    (isinstance(dependencies, list) and
                     len(dependencies) != len(set(dependencies)))):
                errors.append(f"{gap_where}.dependencies: expected nonempty unique IDs")
        if len(gap_ids) != len(set(gap_ids)):
            errors.append("ledger.audit.gaps: duplicate gap ID")
    rows = document.get("rows")
    tombstones = document.get("tombstones")
    if not isinstance(rows, list):
        errors.append("ledger.rows: expected array")
        rows = []
    if not isinstance(tombstones, list):
        errors.append("ledger.tombstones: expected array")
        tombstones = []

    live_ids: list[str] = []
    for index, row in enumerate(rows):
        where = f"ledger.rows[{index}]"
        if not _exact_keys(
            row,
            {
                "id", "title", "delivery_state", "claim_id", "owned_paths",
                "classification", "evidence", "surfaces"
            },
            {"release"}, where, errors
        ):
            continue
        row_id = row.get("id")
        if not _is_nonempty_string(row_id) or not ID_RE.fullmatch(row_id):
            errors.append(f"{where}.id: invalid identifier")
        else:
            live_ids.append(row_id)
        if not _is_nonempty_string(row.get("title")):
            errors.append(f"{where}.title: must be nonempty")
        delivery_state = row.get("delivery_state")
        if delivery_state not in DELIVERY_STATES:
            errors.append(f"{where}.delivery_state: expected pending or released")
        if not _is_nonempty_string(row.get("claim_id")):
            errors.append(f"{where}.claim_id: must be nonempty")
        owned_paths = row.get("owned_paths")
        if (not isinstance(owned_paths, list) or not owned_paths or
                any(not _is_nonempty_string(path) for path in owned_paths)):
            errors.append(f"{where}.owned_paths: expected nonempty unique paths")
        else:
            if len(owned_paths) != len(set(owned_paths)):
                errors.append(f"{where}.owned_paths: duplicate path")
            for path_index, path in enumerate(owned_paths):
                _safe_repo_path(
                    repo_root, path, f"{where}.owned_paths[{path_index}]", errors
                )
        if row.get("classification") not in CLASSIFICATIONS:
            errors.append(f"{where}.classification: missing or invalid classification")
        release = row.get("release")
        if delivery_state == "pending" and release is not None:
            errors.append(f"{where}.release: pending row cannot claim unknowable merge evidence")
        if delivery_state == "released" and release is None:
            errors.append(f"{where}.release: released row requires protected merge evidence")
        if release is not None and _exact_keys(
            release, {"pr", "accepted_head", "merge_sha", "integration_mode"}, set(),
            f"{where}.release", errors
        ):
            if not isinstance(release.get("pr"), int) or isinstance(release.get("pr"), bool) or release["pr"] < 1:
                errors.append(f"{where}.release.pr: expected positive integer")
            for field in ("accepted_head", "merge_sha"):
                if not isinstance(release.get(field), str) or not SHA_RE.fullmatch(release[field]):
                    errors.append(f"{where}.release.{field}: expected lowercase 40-hex SHA")
            if release.get("integration_mode") not in {"merge", "squash"}:
                errors.append(f"{where}.release.integration_mode: expected merge or squash")
        evidence = row.get("evidence")
        if not isinstance(evidence, list) or not evidence:
            errors.append(f"{where}.evidence: expected nonempty array")
        else:
            for item_index, item in enumerate(evidence):
                _validate_evidence(
                    item, repo_root, f"{where}.evidence[{item_index}]", errors,
                    require_kind=False,
                    validate_current_path=delivery_state == "pending",
                )
        surfaces = row.get("surfaces")
        if not _exact_keys(surfaces, SURFACES, set(), f"{where}.surfaces", errors):
            continue
        for surface_name in sorted(SURFACES):
            _validate_surface(
                surfaces[surface_name], surface_name, repo_root,
                f"{where}.surfaces.{surface_name}", errors,
                validate_current_paths=delivery_state == "pending",
                profiles=writer_profiles,
                refusal_codes=writer_refusal_codes,
            )

    if len(live_ids) != len(set(live_ids)):
        errors.append("ledger.rows: duplicate live row ID")

    dead_ids: list[str] = []
    replacements: list[tuple[str, str]] = []
    for index, tombstone in enumerate(tombstones):
        where = f"ledger.tombstones[{index}]"
        if not _exact_keys(
            tombstone,
            {"id", "delivery_state", "claim_id", "owned_paths", "rationale"},
            {"replaced_by", "removed_in_merge"},
            where, errors
        ):
            continue
        dead_id = tombstone.get("id")
        if not _is_nonempty_string(dead_id) or not ID_RE.fullmatch(dead_id):
            errors.append(f"{where}.id: invalid identifier")
        else:
            dead_ids.append(dead_id)
        if not _is_nonempty_string(tombstone.get("rationale")):
            errors.append(f"{where}.rationale: must be nonempty")
        if not _is_nonempty_string(tombstone.get("claim_id")):
            errors.append(f"{where}.claim_id: must be nonempty")
        owned_paths = tombstone.get("owned_paths")
        if (not isinstance(owned_paths, list) or not owned_paths or
                any(not _is_nonempty_string(path) for path in owned_paths)):
            errors.append(f"{where}.owned_paths: expected nonempty unique paths")
        else:
            if len(owned_paths) != len(set(owned_paths)):
                errors.append(f"{where}.owned_paths: duplicate path")
            for path_index, path in enumerate(owned_paths):
                _safe_repo_path(
                    repo_root, path, f"{where}.owned_paths[{path_index}]", errors
                )
        state = tombstone.get("delivery_state")
        removed_in_merge = tombstone.get("removed_in_merge")
        if state not in DELIVERY_STATES:
            errors.append(f"{where}.delivery_state: expected pending or released")
        if state == "pending" and removed_in_merge is not None:
            errors.append(
                f"{where}.removed_in_merge: pending tombstone cannot claim unknowable merge"
            )
        if state == "released" and removed_in_merge is None:
            errors.append(f"{where}.removed_in_merge: released tombstone requires merge proof")
        if removed_in_merge is not None and (
            not isinstance(removed_in_merge, str) or not SHA_RE.fullmatch(removed_in_merge)
        ):
            errors.append(f"{where}.removed_in_merge: expected lowercase 40-hex SHA")
        replacement = tombstone.get("replaced_by")
        if replacement is not None:
            if not _is_nonempty_string(replacement) or not ID_RE.fullmatch(replacement):
                errors.append(f"{where}.replaced_by: invalid identifier")
            else:
                replacements.append((where, replacement))
    if len(dead_ids) != len(set(dead_ids)):
        errors.append("ledger.tombstones: duplicate tombstone ID")
    reused = set(live_ids) & set(dead_ids)
    if reused:
        errors.append(f"ledger: tombstoned IDs reused as live rows: {', '.join(sorted(reused))}")
    known_ids = set(live_ids) | set(dead_ids)
    for where, replacement in replacements:
        if replacement not in known_ids:
            errors.append(f"{where}.replaced_by: unknown ledger ID {replacement!r}")
    return errors


def validate_dependency_references(document: Any) -> list[str]:
    """Optionally validate active dependency IDs against the ledger itself.

    Dependency strings historically include external project and architecture
    references, so the ordinary ledger gate deliberately checks only shape. This
    opt-in pass is for reconciliation work: pending rows must point at another
    live row or an explicit tombstone, while released history remains untouched.
    """
    rows = document.get("rows", []) if isinstance(document, dict) else []
    tombstones = document.get("tombstones", []) if isinstance(document, dict) else []
    known = {
        item.get("id")
        for item in [*rows, *tombstones]
        if isinstance(item, dict) and _is_nonempty_string(item.get("id"))
    }
    errors: list[str] = []
    for row_index, row in enumerate(rows):
        if not isinstance(row, dict) or row.get("delivery_state") != "pending":
            continue
        surfaces = row.get("surfaces", {})
        if not isinstance(surfaces, dict):
            continue
        for surface_name, surface in surfaces.items():
            if not isinstance(surface, dict):
                continue
            dependencies = surface.get("dependencies", [])
            if not isinstance(dependencies, list):
                continue
            for dependency in dependencies:
                if dependency not in known:
                    errors.append(
                        f"ledger.rows[{row_index}].surfaces.{surface_name}.dependencies: "
                        f"active dependency {dependency!r} has no ledger row or tombstone"
                    )
    return errors


def validate_schema_contract(schema: Any) -> list[str]:
    """Detect drift between the checked-in JSON Schema and this dependency-free gate."""
    errors: list[str] = []
    try:
        properties = schema["properties"]
        definitions = schema["$defs"]
        row_properties = definitions["row"]["properties"]
        surface_properties = definitions["surface"]["properties"]
        evidence_properties = definitions["evidence"]["properties"]
    except (KeyError, TypeError) as error:
        return [f"schema contract: missing structural field {error}"]
    expected_top = {"schema_version", "ledger_id", "audit", "rows", "tombstones"}
    if set(schema.get("required", [])) != expected_top or set(properties) != expected_top:
        errors.append("schema contract: top-level fields drifted from checker")
    if set(row_properties.get("classification", {}).get("enum", [])) != CLASSIFICATIONS:
        errors.append("schema contract: classification enum drifted from checker")
    if set(row_properties.get("delivery_state", {}).get("enum", [])) != DELIVERY_STATES:
        errors.append("schema contract: delivery-state enum drifted from checker")
    if set(surface_properties.get("disposition", {}).get("enum", [])) != DISPOSITIONS:
        errors.append("schema contract: disposition enum drifted from checker")
    if set(evidence_properties.get("kind", {}).get("enum", [])) != EVIDENCE_KINDS:
        errors.append("schema contract: evidence-kind enum drifted from checker")
    authority_definition = definitions.get("authority", {})
    authority_properties = authority_definition.get("properties", {})
    if set(authority_properties) != REQUIRED_AUTHORITY_KEYS:
        errors.append("schema contract: authority fields drifted from checker")
    if set(authority_definition.get("required", [])) != REQUIRED_AUTHORITY_KEYS:
        errors.append("schema contract: required authority fields drifted from checker")
    if set(authority_properties.get("admission", {}).get("enum", [])) != ADMISSIONS:
        errors.append("schema contract: admission enum drifted from checker")
    if "authority" not in surface_properties:
        errors.append("schema contract: surface must carry an authority descriptor")
    expected_row_fields = {
        "id", "title", "delivery_state", "claim_id", "owned_paths",
        "classification", "evidence", "surfaces"
    }
    if set(definitions["row"].get("required", [])) != expected_row_fields:
        errors.append("schema contract: required row fields drifted from checker")
    expected_release_fields = {
        "pr", "accepted_head", "merge_sha", "integration_mode"
    }
    if set(definitions.get("release", {}).get("required", [])) != expected_release_fields:
        errors.append("schema contract: required release fields drifted from checker")
    expected_tombstone_fields = {
        "id", "delivery_state", "claim_id", "owned_paths", "rationale"
    }
    tombstone_schema = definitions.get("tombstone", {})
    if set(tombstone_schema.get("required", [])) != expected_tombstone_fields:
        errors.append("schema contract: required tombstone fields drifted from checker")
    tombstone_rules = tombstone_schema.get("allOf", [])
    pending_rule = next(
        (
            rule for rule in tombstone_rules
            if rule.get("if", {}).get("properties", {}).get("delivery_state", {}).get("const")
            == "pending"
        ),
        {},
    )
    released_rule = next(
        (
            rule for rule in tombstone_rules
            if rule.get("if", {}).get("properties", {}).get("delivery_state", {}).get("const")
            == "released"
        ),
        {},
    )
    if pending_rule.get("then", {}).get("not", {}).get("required") != ["removed_in_merge"]:
        errors.append("schema contract: pending tombstone must forbid removed_in_merge")
    if set(released_rule.get("then", {}).get("required", [])) != {"removed_in_merge"}:
        errors.append("schema contract: released tombstone must require removed_in_merge")
    try:
        schema_surfaces = set(definitions["row"]["properties"]["surfaces"]["required"])
    except (KeyError, TypeError):
        schema_surfaces = set()
    if schema_surfaces != SURFACES:
        errors.append("schema contract: required surfaces drifted from checker")
    gap_rule = None
    for rule in definitions.get("surface", {}).get("allOf", []):
        dispositions = (
            rule.get("if", {})
            .get("properties", {})
            .get("disposition", {})
            .get("enum", [])
        )
        if set(dispositions) == {"gap", "deferred"}:
            gap_rule = rule
            break
    exposed_rule = None
    for rule in definitions.get("surface", {}).get("allOf", []):
        if rule.get("if", {}).get("properties", {}).get("disposition", {}).get("const") == "exposed":
            exposed_rule = rule
            break
    exposed_then = exposed_rule.get("then", {}) if isinstance(exposed_rule, dict) else {}
    if set(exposed_then.get("required", [])) != {"evidence", "authority"}:
        errors.append(
            "schema contract: exposed must require evidence and an authority descriptor"
        )
    gap_then = gap_rule.get("then", {}) if isinstance(gap_rule, dict) else {}
    dependency_floor = (
        gap_then.get("properties", {}).get("dependencies", {}).get("minItems")
    )
    if set(gap_then.get("required", [])) != {"owner", "dependencies"} or dependency_floor != 1:
        errors.append(
            "schema contract: gap/deferred must require owner and nonempty dependencies"
        )
    return errors


def _row_evidence_items(row: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
    items: list[tuple[str, dict[str, Any]]] = []
    for index, item in enumerate(row.get("evidence", [])):
        if isinstance(item, dict):
            items.append((f"evidence[{index}]", item))
    surfaces = row.get("surfaces", {})
    if isinstance(surfaces, dict):
        for surface_name, surface in surfaces.items():
            if not isinstance(surface, dict):
                continue
            for index, item in enumerate(surface.get("evidence", [])):
                if isinstance(item, dict):
                    items.append((f"surfaces.{surface_name}.evidence[{index}]", item))
    return items


def _read_commit_path(
    repo_root: Path, revision: str, path: str, source: str
) -> GitComparisonProvenance:
    comparison = resolve_git_comparison(
        repo_root, revision, revision, source=source
    )
    return read_git_path(repo_root, comparison, path)


def validate_release_evidence(document: Any, repo_root: Path) -> list[str]:
    """Bind every released evidence claim to the protected shipped snapshot."""
    errors: list[str] = []
    if not isinstance(document, dict) or not isinstance(document.get("rows"), list):
        return errors
    for row_index, row in enumerate(document["rows"]):
        if not isinstance(row, dict) or row.get("delivery_state") != "released":
            continue
        release = row.get("release")
        merge = release.get("merge_sha") if isinstance(release, dict) else None
        if not isinstance(merge, str) or not SHA_RE.fullmatch(merge):
            continue
        for suffix, item in _row_evidence_items(row):
            path = item.get("path")
            needles = item.get("needles")
            if not isinstance(path, str) or not isinstance(needles, list):
                continue
            where = f"ledger.rows[{row_index}].{suffix}"
            snapshot = _read_commit_path(
                repo_root, merge, path, "sequencer_release_evidence"
            )
            if snapshot.status == "path_absent":
                errors.append(
                    f"{where}: evidence path was absent from released snapshot {merge}: {path}"
                )
                continue
            if snapshot.status != "available":
                errors.append(
                    f"{where}: released evidence history unavailable; receipt="
                    + git_comparison_receipt(snapshot)
                )
                continue
            for needle in needles:
                if isinstance(needle, str) and needle not in (snapshot.content or ""):
                    errors.append(
                        f"{where}: historical evidence {needle!r} was absent from "
                        f"{path} at released snapshot {merge}"
                    )
    return errors


def validate_git_provenance(
    document: Any, repo_root: Path, head: str = "HEAD"
) -> list[str]:
    """Prove released coordinates are real protected merge commits in this checkout."""
    errors: list[str] = []
    if not isinstance(document, dict) or not isinstance(document.get("rows"), list):
        return errors
    for index, row in enumerate(document["rows"]):
        if not isinstance(row, dict) or row.get("delivery_state") != "released":
            continue
        release = row.get("release")
        if not isinstance(release, dict):
            continue
        where = f"ledger.rows[{index}].release"
        accepted = release.get("accepted_head")
        merge = release.get("merge_sha")
        mode = release.get("integration_mode")
        pr = release.get("pr")
        if (not isinstance(accepted, str) or not isinstance(merge, str) or
                mode not in {"merge", "squash"} or not isinstance(pr, int)):
            continue
        merge_object = _git(repo_root, "cat-file", "-e", f"{merge}^{{commit}}")
        if merge_object.returncode != 0:
            errors.append(f"{where}.merge_sha: commit object is unavailable: {merge}")
        accepted_object = _git(repo_root, "cat-file", "-e", f"{accepted}^{{commit}}")
        if accepted_object.returncode != 0 and mode == "squash":
            fetched = _git(
                repo_root, "fetch", "--no-tags", "--quiet", "origin",
                f"refs/pull/{pr}/head"
            )
            fetched_head = _git(repo_root, "rev-parse", "FETCH_HEAD")
            if (fetched.returncode != 0 or fetched_head.returncode != 0 or
                    fetched_head.stdout.strip() != accepted):
                detail = fetched.stderr.strip() or "pull ref did not match recorded head"
                errors.append(
                    f"{where}.accepted_head: exact refs/pull/{pr}/head fetch failed: {detail}"
                )
            accepted_object = _git(repo_root, "cat-file", "-e", f"{accepted}^{{commit}}")
        if accepted_object.returncode != 0:
            errors.append(f"{where}.accepted_head: commit object is unavailable: {accepted}")
        parents = _git(repo_root, "rev-list", "--parents", "-n", "1", merge)
        parent_ids = parents.stdout.strip().split() if parents.returncode == 0 else []
        if mode == "merge":
            if len(parent_ids) != 3:
                errors.append(f"{where}.merge_sha: merge mode requires exactly two parents")
            elif parent_ids[2] != accepted:
                errors.append(
                    f"{where}: accepted_head is not the merge commit's second parent"
                )
            message = _git(repo_root, "log", "-1", "--format=%B", merge)
            subject = message.stdout.splitlines()[0] if message.stdout.splitlines() else ""
            if (message.returncode != 0 or
                    not subject.startswith(f"Merge pull request #{pr} ")):
                errors.append(
                    f"{where}.merge_sha: merge commit message does not bind PR #{pr}"
                )
        elif mode == "squash":
            if len(parent_ids) != 2:
                errors.append(f"{where}.merge_sha: squash mode requires exactly one parent")
            message = _git(repo_root, "log", "-1", "--format=%B", merge)
            if message.returncode != 0 or f"(#{pr})" not in message.stdout:
                errors.append(f"{where}.merge_sha: squash commit message does not reference (#{pr})")
            for path in row.get("owned_paths", []):
                if not isinstance(path, str):
                    continue
                accepted_blob = _git(repo_root, "rev-parse", f"{accepted}:{path}")
                merge_blob = _git(repo_root, "rev-parse", f"{merge}:{path}")
                accepted_exists = accepted_blob.returncode == 0
                merge_exists = merge_blob.returncode == 0
                if accepted_exists != merge_exists:
                    errors.append(f"{where}: squash deletion/presence mismatch for {path}")
                elif accepted_exists and accepted_blob.stdout.strip() != merge_blob.stdout.strip():
                    errors.append(f"{where}: squash blob differs from accepted head for {path}")
        ancestry = _git(repo_root, "merge-base", "--is-ancestor", merge, head)
        problem = _ancestry_problem(
            ancestry,
            f"{where}.merge_sha: merge is not an ancestor of HEAD",
            f"{where}.merge_sha: ancestry history unavailable",
        )
        if problem:
            errors.append(problem)
    return errors


def validate_tombstone_provenance(
    document: Any, repo_root: Path, head: str = "HEAD"
) -> list[str]:
    """Prove released removals name a real commit in protected history."""
    errors: list[str] = []
    if not isinstance(document, dict) or not isinstance(document.get("tombstones"), list):
        return errors
    for index, tombstone in enumerate(document["tombstones"]):
        if (not isinstance(tombstone, dict) or
                tombstone.get("delivery_state") != "released"):
            continue
        merge = tombstone.get("removed_in_merge")
        if not isinstance(merge, str) or not SHA_RE.fullmatch(merge):
            continue
        where = f"ledger.tombstones[{index}].removed_in_merge"
        commit = _git(repo_root, "cat-file", "-e", f"{merge}^{{commit}}")
        if commit.returncode != 0:
            errors.append(f"{where}: commit object is unavailable: {merge}")
            continue
        ancestry = _git(repo_root, "merge-base", "--is-ancestor", merge, head)
        problem = _ancestry_problem(
            ancestry,
            f"{where}: removal commit is not an ancestor of HEAD",
            f"{where}: removal ancestry history unavailable",
        )
        if problem:
            errors.append(problem)
            continue
        comparison = resolve_git_comparison(
            repo_root, merge, merge, source="sequencer_tombstone_snapshot"
        )
        snapshot_ledger, _, snapshot_errors, snapshot = _read_ledger_at_anchor(
            repo_root, comparison
        )
        if snapshot.status == "path_absent":
            errors.append(
                f"{where}: removal snapshot does not contain the ledger"
            )
            continue
        if snapshot.status != "available":
            errors.append(
                f"{where}: removal snapshot history unavailable; receipt="
                + git_comparison_receipt(snapshot)
            )
            continue
        if snapshot_errors or not isinstance(snapshot_ledger, dict):
            for problem in snapshot_errors or ["removal snapshot ledger is unreadable"]:
                errors.append(f"{where}: {problem}")
            continue
        tombstone_id = tombstone.get("id")
        snapshot_live_ids = {
            row.get("id")
            for row in snapshot_ledger.get("rows", [])
            if isinstance(row, dict)
        }
        if tombstone_id in snapshot_live_ids:
            errors.append(f"{where}: removal snapshot still contains live row {tombstone_id}")
        snapshot_tombstones = {
            item.get("id"): item
            for item in snapshot_ledger.get("tombstones", [])
            if isinstance(item, dict)
        }
        pending = snapshot_tombstones.get(tombstone_id)
        expected_identity = {
            key: value
            for key, value in tombstone.items()
            if key not in {"delivery_state", "removed_in_merge"}
        }
        pending_identity = {
            key: value
            for key, value in pending.items()
            if key not in {"delivery_state", "removed_in_merge"}
        } if isinstance(pending, dict) else None
        if (not isinstance(pending, dict) or pending.get("delivery_state") != "pending" or
                pending_identity != expected_identity):
            errors.append(
                f"{where}: removal snapshot lacks the matching pending tombstone identity"
            )
    return errors


def is_sequencer_owned_path(path: str) -> bool:
    """Return whether a changed path participates in the sequencer program."""
    lowered = path.lower()
    if any(
        token in lowered
        for token in (
            "timeline", "sequencer", "playback", "step_grid", "step-grid",
            "midi_clock", "midi-clock", "clock_chaser", "clock-chaser",
        )
    ):
        return True
    exact = {
        "tools/cli/cmd_seq.cpp",
        "tools/cli/cmd_render.cpp",
        "tools/mcp/mcp_timeline_tools.cpp",
        "tools/mcp/timeline_mcp_tools.h.in",
        "tools/mcp/timeline_session_store.cpp",
        "tools/mcp/timeline_session_store.hpp",
        ".agents/skills/timeline/SKILL.md",
        ".agents/skills/playback/SKILL.md",
    }
    prefixes = (
        "core/timeline/",
        "core/playback/",
        "tools/timeline/",
        "examples/timeline",
    )
    if path in exact or path.startswith(prefixes):
        return True
    if path.startswith("core/host/") and "timeline" in path.lower():
        return True
    if path.startswith("test/") and any(
        token in Path(path).name.lower() for token in ("timeline", "playback", "sequencer")
    ):
        return True
    if path.startswith("docs/") and any(
        token in path.lower() for token in ("timeline", "sequencer", "playback")
    ):
        return True
    return path.startswith(".github/workflows/") and any(
        token in Path(path).name.lower() for token in ("timeline", "sequencer", "playback")
    )


def _rows_by_id(document: Any) -> dict[str, dict[str, Any]]:
    if not isinstance(document, dict) or not isinstance(document.get("rows"), list):
        return {}
    return {
        row["id"]: row
        for row in document["rows"]
        if isinstance(row, dict) and _is_nonempty_string(row.get("id"))
    }


def _is_build_manifest(path: str) -> bool:
    """Return whether a path is a CMake registration manifest.

    Every slice that registers a target or a test appends to the same manifest,
    so no ledger row owns one whole: a row owns the line it registered, and its
    evidence needles are the instrument for a line. The name-based predicate
    still watches a manifest named for the sequencer.
    """
    return PurePosixPath(path).name == "CMakeLists.txt" or path.endswith(".cmake")


def _declared_owners(document: Any) -> dict[str, set[str]]:
    """Map each owned path to the ids of the live rows and tombstones declaring it."""
    owners: dict[str, set[str]] = {}
    for row_id, row in _rows_by_id(document).items():
        for path in row.get("owned_paths", []):
            if isinstance(path, str):
                owners.setdefault(path, set()).add(row_id)
    if isinstance(document, dict) and isinstance(document.get("tombstones"), list):
        for tombstone in document["tombstones"]:
            if not isinstance(tombstone, dict) or not _is_nonempty_string(tombstone.get("id")):
                continue
            for path in tombstone.get("owned_paths", []):
                if isinstance(path, str):
                    owners.setdefault(path, set()).add(tombstone["id"])
    return owners


def _exclusively_owned_paths(base: Any | None, current: Any) -> set[str]:
    """Paths the ledger watches whole.

    A path is watched when exactly one row or tombstone declares it in every
    ledger state the transition spans and it is not a registration manifest. Two
    declaring owners mean the path is shared by construction, and a row that
    only proves a line in a file lists it as evidence, not as an owned path. A
    path that a transition drops from its only owner stays watched for that
    transition, so de-annexation is its own reviewable ledger change rather
    than a way to change a file unrecorded. A wholesale-regenerated artifact is
    never watched whole: another required gate compels its refresh, so watching
    it would leave the author no way to satisfy both.
    """
    base_owners = _declared_owners(base)
    current_owners = _declared_owners(current)
    return {
        path
        for path in set(base_owners) | set(current_owners)
        if len(base_owners.get(path, ())) <= 1
        and len(current_owners.get(path, ())) <= 1
        and not _is_build_manifest(path)
        and path not in GENERATED_ARTIFACT_PATHS
    }


def _material_row(row: dict[str, Any]) -> dict[str, Any]:
    """Fields that make a row a substantive exposure transaction."""
    return {
        key: row.get(key)
        for key in (
            "delivery_state", "claim_id", "owned_paths", "classification",
            "evidence", "surfaces"
        )
    }


def validate_transition(
    base: Any | None,
    current: Any,
    changed_paths: list[str],
    trailer_ids: list[str] | None = None,
    semantic_added_paths: set[str] | None = None,
    mechanical_version_paths: set[str] | None = None,
) -> list[str]:
    """Validate omission prevention and append-only removal history."""
    errors: list[str] = []
    changed = set(changed_paths)
    base_document = base if isinstance(base, dict) else {}
    current_document = current if isinstance(current, dict) else {}
    base_rows = _rows_by_id(base)
    current_rows = _rows_by_id(current)
    base_tombstones = {
        row.get("id"): row for row in base_document.get("tombstones", [])
        if isinstance(row, dict) and _is_nonempty_string(row.get("id"))
    }
    current_tombstones = {
        row.get("id"): row for row in current_document.get("tombstones", [])
        if isinstance(row, dict) and _is_nonempty_string(row.get("id"))
    }
    watched = _exclusively_owned_paths(base, current)
    sequencer_changes = sorted(
        path for path in changed
        if not is_ledger_path(path) and path not in E0_INFRASTRUCTURE_PATHS and
        (
            is_sequencer_owned_path(path) or
            path in (semantic_added_paths or set()) or
            (
                path in watched
                and path not in (mechanical_version_paths or set())
            )
        )
    )
    changed_rows = {
        row_id: row
        for row_id, row in current_rows.items()
        if row_id not in base_rows or _material_row(row) != _material_row(base_rows[row_id])
    }
    for path in sequencer_changes:
        row_covering = [
            row_id
            for row_id, row in changed_rows.items()
            if row.get("delivery_state") == "pending" and path in row.get("owned_paths", [])
        ]
        covering = list(row_covering)
        covering.extend(
            tombstone_id
            for tombstone_id, tombstone in current_tombstones.items()
            if tombstone_id not in base_tombstones and
            tombstone.get("delivery_state") == "pending" and
            path in tombstone.get("owned_paths", [])
        )
        if not covering:
            errors.append(
                "transition: sequencer-owned changed path is not covered by an added or "
                f"materially changed pending row: {path}"
            )
        if path in (semantic_added_paths or set()) and not (
            set(row_covering) & set(trailer_ids or [])
        ):
            errors.append(
                "transition: cross-module semantic path requires a Sequencer-Exposure "
                f"trailer naming its covering pending row: {path}"
            )
    for trailer_id in trailer_ids or []:
        row = changed_rows.get(trailer_id)
        if row is None or row.get("delivery_state") != "pending":
            errors.append(
                f"transition: Sequencer-Exposure trailer lacks a changed pending row: {trailer_id}"
            )
    if not isinstance(base, dict):
        return errors
    for row_id, old_tombstone in sorted(base_tombstones.items()):
        if row_id not in current_tombstones:
            errors.append(f"transition: append-only tombstone removed: {row_id}")
            continue
        new_tombstone = current_tombstones[row_id]
        if old_tombstone.get("delivery_state") == "released":
            if new_tombstone != old_tombstone:
                errors.append(f"transition: published tombstone changed: {row_id}")
            continue
        old_identity = {
            key: value
            for key, value in old_tombstone.items()
            if key not in {"delivery_state", "removed_in_merge"}
        }
        new_identity = {
            key: value
            for key, value in new_tombstone.items()
            if key not in {"delivery_state", "removed_in_merge"}
        }
        promoted = (
            old_tombstone.get("delivery_state") == "pending" and
            new_tombstone.get("delivery_state") == "released" and
            old_identity == new_identity and
            _is_nonempty_string(new_tombstone.get("removed_in_merge"))
        )
        if new_tombstone != old_tombstone and not promoted:
            errors.append(f"transition: pending tombstone changed outside release promotion: {row_id}")
    for row_id in sorted(set(base_rows) - set(current_rows)):
        tombstone = current_tombstones.get(row_id)
        if (tombstone is None or row_id in base_tombstones or
                tombstone.get("delivery_state") != "pending"):
            errors.append(
                f"transition: removed live row lacks a newly added pending tombstone: {row_id}"
            )
        elif tombstone.get("owned_paths") != base_rows[row_id].get("owned_paths"):
            errors.append(
                f"transition: removal tombstone must preserve exact owned_paths: {row_id}"
            )
    return errors


def _git(repo_root: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(repo_root), *arguments],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def _ancestry_problem(
    result: subprocess.CompletedProcess[str], negative: str, unavailable: str
) -> str | None:
    if result.returncode == 0:
        return None
    if result.returncode == 1:
        return negative
    detail = " ".join(result.stderr.strip().split())[:512]
    return f"{unavailable} (git exit {result.returncode}): {detail}"


def _load_base_transition_with_receipt(
    repo_root: Path,
    base: str,
    comparison: GitComparisonProvenance | None = None,
) -> tuple[Any | None, list[str], list[str], GitComparisonProvenance]:
    errors: list[str] = []
    comparison = comparison or resolve_git_comparison(
        repo_root, base, source="sequencer_cli_base"
    )
    if comparison.status != "available" or comparison.comparison_anchor is None:
        return None, [], [
            "transition: git history unavailable; comparison receipt="
            + git_comparison_receipt(comparison)
        ], comparison
    anchor = comparison.comparison_anchor
    # Disable rename coalescing so both the deleted and added endpoints are
    # governed, including a rename that crosses out of a watched subtree.
    diff = _git(
        repo_root,
        "diff",
        "--name-only",
        "--no-renames",
        "-z",
        "--diff-filter=ACDMRTUXB",
        anchor,
        comparison.resolved_head or "HEAD",
    )
    if diff.returncode != 0:
        return None, [], [
            f"transition: cannot diff proven base {anchor!r}: "
            f"{' '.join(diff.stderr.strip().split())[:512]}"
        ], comparison
    changed_paths = [path for path in diff.stdout.split("\0") if path]
    base_document, _, ledger_errors, shown = _read_ledger_at_anchor(repo_root, comparison)
    if shown.status == "path_absent":
        # Bootstrap is valid only because the current ledger itself is new in the diff.
        if not _ledger_fragment_paths(changed_paths):
            errors.append(
                "transition: base carries no ledger and this change does not add one"
            )
        # The comparison itself resolved; only the ledger is absent from it. The
        # caller still needs a usable anchor to read trailers and added semantics.
        return None, changed_paths, errors, comparison
    if shown.status != "available":
        errors.append(
            "transition: base ledger history unavailable; comparison receipt="
            + git_comparison_receipt(shown)
        )
        return None, changed_paths, errors, shown
    errors.extend(f"transition: base {problem}" for problem in ledger_errors)
    if ledger_errors:
        base_document = None
    return base_document, changed_paths, errors, shown


def _load_base_transition(repo_root: Path, base: str) -> tuple[Any | None, list[str], list[str]]:
    """Compatibility wrapper for focused unit tests and existing callers."""
    document, paths, errors, _ = _load_base_transition_with_receipt(repo_root, base)
    return document, paths, errors


def _load_sequencer_trailers(
    repo_root: Path, base: str, head: str = "HEAD"
) -> tuple[list[str], list[str]]:
    log = _git(repo_root, "log", "--format=%B%x00", f"{base}..{head}")
    if log.returncode != 0:
        return [], [f"transition: cannot inspect commit trailers: {log.stderr.strip()}"]
    trailer_ids: list[str] = []
    errors: list[str] = []
    for line in log.stdout.replace("\x00", "\n").splitlines():
        if not line.startswith("Sequencer-Exposure:"):
            continue
        value = line.partition(":")[2].strip()
        if not value or not ID_RE.fullmatch(value):
            errors.append(f"transition: malformed Sequencer-Exposure trailer: {line!r}")
        else:
            trailer_ids.append(value)
    return trailer_ids, errors


def _mechanical_version_paths(
    repo_root: Path, base: str, changed_paths: list[str], head: str = "HEAD"
) -> tuple[set[str], list[str]]:
    """Find declared artifacts whose diff is confined to a version-string rewrite.

    The version bot regenerates these files on every bump, so their whole-file
    watch would make the bump PR unsatisfiable: a mechanical version rewrite has
    no exposure decision to record, and no ledger row can honestly cover it. The
    scan is deliberately per-transition rather than per-path, so the exemption
    lasts exactly as long as the diff stays mechanical.
    """
    matched: set[str] = set()
    errors: list[str] = []
    for path in changed_paths:
        keys = MECHANICAL_VERSION_ARTIFACTS.get(path)
        if not keys:
            continue
        diff = _git(
            repo_root,
            "diff",
            "--no-renames",
            "--no-ext-diff",
            "--unified=0",
            f"{base}...{head}",
            "--",
            path,
        )
        if diff.returncode != 0:
            errors.append(
                f"transition: cannot scan version-only change in {path}: "
                f"{diff.stderr.strip()}"
            )
            continue
        body = [
            line
            for line in diff.stdout.splitlines()
            if line[:1] in {"+", "-"} and not line.startswith(("+++", "---"))
        ]
        if not body:
            continue
        mechanical = True
        for line in body:
            field = _VERSION_LINE_RE.match(line[1:].strip().rstrip(","))
            if (
                field is None
                or field.group(1) not in keys
                or _VERSION_VALUE_RE.match(field.group(2)) is None
            ):
                mechanical = False
                break
        if mechanical:
            matched.add(path)
    return matched, errors


def _semantic_added_paths(
    repo_root: Path, base: str, changed_paths: list[str], head: str = "HEAD"
) -> tuple[set[str], list[str]]:
    """Find new cross-module sequencer semantics in otherwise generic paths."""
    candidate_prefixes = ("core/state/", "core/view/", "core/midi/", "inspect/")
    markers = (
        "timeline", "sequencer", "playback", "step_grid", "step-grid",
        "midi_clock", "midi-clock", "clock_chaser", "clock-chaser",
    )
    matched: set[str] = set()
    errors: list[str] = []
    for path in changed_paths:
        if not path.startswith(candidate_prefixes):
            continue
        diff = _git(
            repo_root,
            "diff",
            "--no-renames",
            "--no-ext-diff",
            "--unified=0",
            f"{base}...{head}",
            "--",
            path,
        )
        if diff.returncode != 0:
            errors.append(
                f"transition: cannot scan added semantics in {path}: {diff.stderr.strip()}"
            )
            continue
        added = "\n".join(
            line[1:]
            for line in diff.stdout.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        ).lower()
        if any(marker in added for marker in markers):
            matched.add(path)
    return matched, errors


def is_ledger_path(path: str) -> bool:
    """Return whether a repository path is part of the ledger itself.

    The ledger records what a change exposes; it is not itself a sequencer
    surface, so a change to it never demands a row covering it. The directory
    form spreads the same document over many files, and every one of them is
    the ledger.
    """
    return (
        path == LEDGER_PATH
        or path == LEDGER_DIR
        or path.startswith(f"{LEDGER_DIR}/")
    )


def _fragment_role(path: str) -> str | None:
    """Classify one ledger file, or None when the path is not a ledger file."""
    if path == LEDGER_PATH:
        return "document"
    if path == LEDGER_HEADER_PATH:
        return "header"
    for directory, role in ((LEDGER_ROWS_DIR, "row"), (LEDGER_TOMBSTONES_DIR, "tombstone")):
        prefix = f"{directory}/"
        if path.startswith(prefix):
            remainder = path[len(prefix):]
            return role if remainder and "/" not in remainder else "stray"
    if path.startswith(f"{LEDGER_DIR}/"):
        return "stray"
    return None


def assemble_ledger(fragments: list[tuple[str, str]]) -> tuple[Any | None, dict[str, str],
                                                               list[str]]:
    """Assemble one ledger document from the files that carry it.

    Returns the document, a map from each assembled row/tombstone position to the
    file it came from, and any assembly error. A repository carrying only the
    single legacy document yields it verbatim, so the split layout adds no
    behaviour to a repository that has not adopted it. Where both forms are
    present the rows are the union: the legacy document keeps the rows already
    merged while a new row lands as its own file. Two sources claiming the same
    row ID, or two headers, are errors rather than a silent winner — a ledger
    that reads differently depending on which copy you look at is worse than one
    that refuses to read.
    """
    errors: list[str] = []
    origins: dict[str, str] = {}
    parsed_by_path: dict[str, Any] = {}
    roles: dict[str, str] = {}
    for path, text in sorted(fragments):
        role = _fragment_role(path)
        if role is None:
            continue
        if role == "stray":
            errors.append(
                f"ledger: {path} is not a ledger file; a row belongs in "
                f"{LEDGER_ROWS_DIR}/<id>.json and a tombstone in "
                f"{LEDGER_TOMBSTONES_DIR}/<id>.json"
            )
            continue
        try:
            parsed = json.loads(text)
        except json.JSONDecodeError as error:
            errors.append(f"ledger: {path} is invalid JSON: {error}")
            continue
        if not isinstance(parsed, dict):
            errors.append(f"ledger: {path} must contain a JSON object")
            continue
        parsed_by_path[path] = parsed
        roles[path] = role

    document_source = parsed_by_path.get(LEDGER_PATH)
    header_source = parsed_by_path.get(LEDGER_HEADER_PATH)
    split_paths = [path for path, role in roles.items() if role in {"row", "tombstone"}]
    if document_source is None and header_source is None and not split_paths:
        if not errors:
            errors.append(
                f"ledger: neither {LEDGER_PATH} nor {LEDGER_DIR}/ carries a ledger"
            )
        return None, origins, errors
    if document_source is not None and header_source is not None:
        errors.append(
            f"ledger: {LEDGER_PATH} and {LEDGER_HEADER_PATH} both carry the ledger header; "
            "exactly one may"
        )
        return None, origins, errors
    if document_source is not None and not split_paths:
        # Nothing has adopted the split layout, so the document is the ledger.
        origins["document"] = LEDGER_PATH
        return document_source, origins, errors

    if header_source is not None:
        document = dict(header_source)
        origins["header"] = LEDGER_HEADER_PATH
        rows: list[Any] = []
        tombstones: list[Any] = []
        for key in ("rows", "tombstones"):
            if key in header_source:
                errors.append(
                    f"ledger: {LEDGER_HEADER_PATH} must not carry {key}; each one is its own "
                    f"file under {LEDGER_DIR}/"
                )
                document.pop(key, None)
    else:
        assert document_source is not None
        document = {key: value for key, value in document_source.items()
                    if key not in {"rows", "tombstones"}}
        origins["header"] = LEDGER_PATH
        rows = []
        tombstones = []
        for key, container in (("rows", rows), ("tombstones", tombstones)):
            existing = document_source.get(key)
            if not isinstance(existing, list):
                errors.append(f"ledger.{key}: expected array in {LEDGER_PATH}")
                continue
            for index, item in enumerate(existing):
                container.append(item)
                origins[f"{key}[{len(container) - 1}]"] = f"{LEDGER_PATH}#{key}[{index}]"

    # Seeded with whatever the single document already carries, so a row that is
    # in both forms is caught rather than silently duplicated.
    seen: dict[tuple[str, Any], str] = {}
    for key, container in (("rows", rows), ("tombstones", tombstones)):
        for item in container:
            if isinstance(item, dict) and _is_nonempty_string(item.get("id")):
                seen.setdefault((key, item["id"]), LEDGER_PATH)
    for path in sorted(split_paths):
        parsed = parsed_by_path[path]
        role = roles[path]
        key = "rows" if role == "row" else "tombstones"
        container = rows if role == "row" else tombstones
        stem = PurePosixPath(path).name
        if not stem.endswith(".json"):
            errors.append(f"ledger: {path} must be a .json file")
            continue
        expected_id = stem[: -len(".json")]
        actual_id = parsed.get("id")
        if actual_id != expected_id:
            errors.append(
                f"ledger: {path} declares id {actual_id!r}; a ledger file is named for the "
                f"row it carries, so its name requires {expected_id!r}"
            )
            continue
        previous = seen.get((key, expected_id))
        if previous is not None:
            errors.append(f"ledger: {expected_id!r} is carried by both {previous} and {path}")
            continue
        seen[(key, expected_id)] = path
        container.append(parsed)
        origins[f"{key}[{len(container) - 1}]"] = path

    for key, container in (("rows", rows), ("tombstones", tombstones)):
        document[key] = container
    return document, origins, errors


def _ledger_fragment_paths(paths: list[str]) -> list[str]:
    return [path for path in paths if _fragment_role(path) is not None]


def load_ledger_from_worktree(
    repo_root: Path, document_path: Path | None = None, directory: Path | None = None
) -> tuple[Any | None, dict[str, str], list[str]]:
    """Read every ledger file present in the working tree and assemble them."""
    document_path = document_path or repo_root / LEDGER_PATH
    directory = directory or repo_root / LEDGER_DIR
    fragments: list[tuple[str, str]] = []
    errors: list[str] = []
    if document_path.is_file():
        try:
            fragments.append((LEDGER_PATH, document_path.read_text(encoding="utf-8")))
        except (OSError, UnicodeDecodeError) as error:
            errors.append(f"ledger: cannot read {document_path}: {error}")
    if directory.is_dir():
        for candidate in sorted(directory.rglob("*")):
            if not candidate.is_file():
                continue
            relative = PurePosixPath(
                LEDGER_DIR, *candidate.relative_to(directory).parts
            ).as_posix()
            try:
                fragments.append((relative, candidate.read_text(encoding="utf-8")))
            except (OSError, UnicodeDecodeError) as error:
                errors.append(f"ledger: cannot read {relative}: {error}")
    document, origins, assembly_errors = assemble_ledger(fragments)
    return document, origins, errors + assembly_errors


def _read_ledger_at_anchor(
    repo_root: Path, comparison: GitComparisonProvenance
) -> tuple[Any | None, dict[str, str], list[str], GitComparisonProvenance]:
    """Assemble the ledger as it stood at a proven git anchor.

    History is immutable, so a snapshot predating the split still carries the
    single document and a snapshot after it carries the directory. Reading both
    forms is therefore permanent, not transitional.
    """
    receipt = read_git_path(repo_root, comparison, LEDGER_PATH)
    fragments: list[tuple[str, str]] = []
    errors: list[str] = []
    if receipt.status == "available":
        fragments.append((LEDGER_PATH, receipt.content or ""))
    elif receipt.status != "path_absent":
        return None, {}, [], receipt
    anchor = comparison.comparison_anchor
    listed = _git(
        repo_root, "ls-tree", "-r", "-z", "--name-only", str(anchor), "--", f"{LEDGER_DIR}/"
    )
    if listed.returncode != 0:
        detail = " ".join(listed.stderr.strip().split())[:512]
        return None, {}, [f"ledger: cannot list {LEDGER_DIR}/ at {anchor}: {detail}"], receipt
    for name in (path for path in listed.stdout.split("\0") if path):
        shown = read_git_path(repo_root, comparison, name)
        if shown.status != "available":
            return None, {}, [], shown
        fragments.append((name, shown.content or ""))
    if not fragments:
        return None, {}, errors, dataclasses.replace(
            receipt, status="path_absent", content=None
        )
    read_paths = [name for name, _ in fragments]
    assembled_path = read_paths[0] if len(read_paths) == 1 else LEDGER_DIR
    document, origins, assembly_errors = assemble_ledger(fragments)
    return document, origins, errors + assembly_errors, dataclasses.replace(
        receipt, status="available", path=assembled_path, content=None
    )


def _print_origin_legend(errors: list[str], origins: dict[str, str]) -> None:
    """Name the file behind every assembled position an error mentions.

    A row is reported by its position in the assembled document, which is not a
    file when the ledger is a directory of rows. Printing the mapping for the
    positions that actually failed keeps the error actionable without making the
    common case noisier.
    """
    if not origins:
        return
    cited = sorted(
        {
            position
            for position in origins
            if position not in {"document", "header"}
            and any(f"ledger.{position}" in error for error in errors)
        }
    )
    if not cited:
        return
    print("sequencer exposure check: the failing rows come from", file=sys.stderr)
    for position in cited:
        print(f"- ledger.{position}: {origins[position]}", file=sys.stderr)


def _load_json(path: Path) -> Any:
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    default_root = Path(__file__).resolve().parents[2]
    parser.add_argument("--repo-root", type=Path, default=default_root)
    parser.add_argument(
        "--ledger", type=Path,
        help="the single legacy ledger document (defaults to docs/status/sequencer-exposure.json)",
    )
    parser.add_argument(
        "--ledger-dir", type=Path,
        help="the per-row ledger directory (defaults to docs/status/sequencer-exposure)",
    )
    parser.add_argument(
        "--schema",
        type=Path,
        help="checked-in schema (defaults to docs/status/sequencer-exposure.schema.json)",
    )
    parser.add_argument(
        "--base",
        help="git revision used to enforce ledger updates and append-only history",
    )
    parser.add_argument(
        "--strict-dependencies",
        action="store_true",
        help="require pending-row dependencies to resolve to a ledger row or tombstone",
    )
    args = parser.parse_args(argv)
    repo_root = args.repo_root.resolve()
    schema_path = args.schema or repo_root / "docs/status/sequencer-exposure.schema.json"
    document, origins, load_errors = load_ledger_from_worktree(
        repo_root, args.ledger, args.ledger_dir
    )
    if load_errors or document is None:
        print("sequencer exposure check: FAILED", file=sys.stderr)
        for error in load_errors or ["ledger: no ledger could be assembled"]:
            print(f"- {error}", file=sys.stderr)
        return 1
    try:
        schema = _load_json(schema_path)
    except (OSError, json.JSONDecodeError) as error:
        print(f"sequencer exposure check: cannot read {schema_path}: {error}", file=sys.stderr)
        return 1
    errors = validate_document(document, repo_root)
    if args.strict_dependencies:
        errors.extend(validate_dependency_references(document))
    errors.extend(validate_schema_contract(schema))
    comparison: GitComparisonProvenance | None = None
    if args.base:
        comparison = resolve_git_comparison(
            repo_root, args.base, source="sequencer_cli_base"
        )
        frozen_head = comparison.resolved_head
        if frozen_head is None:
            errors.append(
                "git history unavailable; comparison receipt="
                + git_comparison_receipt(comparison)
            )
        else:
            errors.extend(validate_git_provenance(document, repo_root, frozen_head))
            errors.extend(
                validate_tombstone_provenance(document, repo_root, frozen_head)
            )
    else:
        errors.extend(validate_git_provenance(document, repo_root))
        errors.extend(validate_tombstone_provenance(document, repo_root))
    errors.extend(validate_release_evidence(document, repo_root))
    if args.base:
        assert comparison is not None
        (
            base_document,
            changed_paths,
            transition_errors,
            comparison,
        ) = _load_base_transition_with_receipt(repo_root, args.base, comparison)
        if comparison.status == "available" and comparison.comparison_anchor:
            comparison_base = comparison.comparison_anchor
            trailer_ids, trailer_errors = _load_sequencer_trailers(
                repo_root, comparison_base, comparison.resolved_head or "HEAD"
            )
            semantic_paths, semantic_errors = _semantic_added_paths(
                repo_root, comparison_base, changed_paths,
                comparison.resolved_head or "HEAD",
            )
            mechanical_paths, mechanical_errors = _mechanical_version_paths(
                repo_root, comparison_base, changed_paths,
                comparison.resolved_head or "HEAD",
            )
        else:
            trailer_ids, trailer_errors = [], []
            semantic_paths, semantic_errors = set(), []
            mechanical_paths, mechanical_errors = set(), []
        errors.extend(transition_errors)
        errors.extend(trailer_errors)
        errors.extend(semantic_errors)
        errors.extend(mechanical_errors)
        errors.extend(
            validate_transition(
                base_document,
                document,
                changed_paths,
                trailer_ids,
                semantic_paths,
                mechanical_paths,
            )
        )
        print("sequencer exposure check: comparison receipt=" +
              git_comparison_receipt(comparison))
    if errors:
        print("sequencer exposure check: FAILED", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        _print_origin_legend(errors, origins)
        return 1
    print(f"sequencer exposure check: OK ({len(document['rows'])} rows, "
          f"{len(document['tombstones'])} tombstones)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
