#!/usr/bin/env python3
"""Fail-closed validation for the sequencer cross-surface exposure ledger."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Any


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
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
LEDGER_PATH = "docs/status/sequencer-exposure.json"
E0_INFRASTRUCTURE_PATHS = {
    LEDGER_PATH,
}


def _is_nonempty_string(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


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


def _validate_surface(
    surface: Any,
    name: str,
    repo_root: Path,
    where: str,
    errors: list[str],
    *,
    validate_current_paths: bool,
) -> set[str]:
    if not _exact_keys(
        surface,
        {"disposition", "rationale"},
        {"owner", "dependencies", "evidence"},
        where,
        errors,
    ):
        return set()
    disposition = surface.get("disposition")
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
    if not _exact_keys(
        document, {"schema_version", "ledger_id", "audit", "rows", "tombstones"}, set(),
        "ledger", errors
    ):
        return errors
    if document.get("schema_version") != 1:
        errors.append("ledger.schema_version: expected 1")
    if document.get("ledger_id") != "dev.pulp.sequencer-exposure@1":
        errors.append("ledger.ledger_id: expected dev.pulp.sequencer-exposure@1")
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
            snapshot = _git(repo_root, "show", f"{merge}:{path}")
            if snapshot.returncode != 0:
                errors.append(
                    f"{where}: evidence path was absent from released snapshot {merge}: {path}"
                )
                continue
            for needle in needles:
                if isinstance(needle, str) and needle not in snapshot.stdout:
                    errors.append(
                        f"{where}: historical evidence {needle!r} was absent from "
                        f"{path} at released snapshot {merge}"
                    )
    return errors


def validate_git_provenance(document: Any, repo_root: Path) -> list[str]:
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
        ancestry = _git(repo_root, "merge-base", "--is-ancestor", merge, "HEAD")
        if ancestry.returncode != 0:
            errors.append(f"{where}.merge_sha: merge is not an ancestor of HEAD")
    return errors


def validate_tombstone_provenance(document: Any, repo_root: Path) -> list[str]:
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
        ancestry = _git(repo_root, "merge-base", "--is-ancestor", merge, "HEAD")
        if ancestry.returncode != 0:
            errors.append(f"{where}: removal commit is not an ancestor of HEAD")
            continue
        snapshot = _git(repo_root, "show", f"{merge}:{LEDGER_PATH}")
        if snapshot.returncode != 0:
            errors.append(f"{where}: removal snapshot does not contain {LEDGER_PATH}")
            continue
        try:
            snapshot_ledger = json.loads(snapshot.stdout)
        except json.JSONDecodeError as error:
            errors.append(f"{where}: removal snapshot ledger is invalid JSON: {error}")
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


def _documented_paths(document: Any) -> set[str]:
    paths: set[str] = set()
    for row in _rows_by_id(document).values():
        for path in row.get("owned_paths", []):
            if isinstance(path, str):
                paths.add(path)
        for item in row.get("evidence", []):
            if isinstance(item, dict) and isinstance(item.get("path"), str):
                paths.add(item["path"])
        surfaces = row.get("surfaces", {})
        if isinstance(surfaces, dict):
            for surface in surfaces.values():
                if not isinstance(surface, dict):
                    continue
                for item in surface.get("evidence", []):
                    if isinstance(item, dict) and isinstance(item.get("path"), str):
                        paths.add(item["path"])
    if isinstance(document, dict) and isinstance(document.get("tombstones"), list):
        for tombstone in document["tombstones"]:
            if not isinstance(tombstone, dict):
                continue
            for path in tombstone.get("owned_paths", []):
                if isinstance(path, str):
                    paths.add(path)
    return paths


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
    documented = _documented_paths(base) | _documented_paths(current)
    sequencer_changes = sorted(
        path for path in changed
        if path not in E0_INFRASTRUCTURE_PATHS and
        (
            is_sequencer_owned_path(path) or path in documented or
            path in (semantic_added_paths or set())
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


def _load_base_transition(repo_root: Path, base: str) -> tuple[Any | None, list[str], list[str]]:
    errors: list[str] = []
    # Disable rename coalescing so both the deleted and added endpoints are
    # governed, including a rename that crosses out of a watched subtree.
    diff = _git(
        repo_root,
        "diff",
        "--name-only",
        "--no-renames",
        "-z",
        "--diff-filter=ACDMRTUXB",
        f"{base}...HEAD",
    )
    if diff.returncode != 0:
        return None, [], [f"transition: cannot diff base {base!r}: {diff.stderr.strip()}"]
    changed_paths = [path for path in diff.stdout.split("\0") if path]
    shown = _git(repo_root, "show", f"{base}:{LEDGER_PATH}")
    if shown.returncode != 0:
        # Bootstrap is valid only because the current ledger itself is new in the diff.
        if LEDGER_PATH not in changed_paths:
            errors.append(
                f"transition: base lacks {LEDGER_PATH} and this change does not add it"
            )
        return None, changed_paths, errors
    try:
        base_document = json.loads(shown.stdout)
    except json.JSONDecodeError as error:
        errors.append(f"transition: base ledger is invalid JSON: {error}")
        base_document = None
    return base_document, changed_paths, errors


def _load_sequencer_trailers(repo_root: Path, base: str) -> tuple[list[str], list[str]]:
    log = _git(repo_root, "log", "--format=%B%x00", f"{base}..HEAD")
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


def _semantic_added_paths(
    repo_root: Path, base: str, changed_paths: list[str]
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
            f"{base}...HEAD",
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


def _load_json(path: Path) -> Any:
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    default_root = Path(__file__).resolve().parents[2]
    parser.add_argument("--repo-root", type=Path, default=default_root)
    parser.add_argument("--ledger", type=Path)
    parser.add_argument(
        "--schema",
        type=Path,
        help="checked-in schema (defaults to docs/status/sequencer-exposure.schema.json)",
    )
    parser.add_argument(
        "--base",
        help="git revision used to enforce ledger updates and append-only history",
    )
    args = parser.parse_args(argv)
    repo_root = args.repo_root.resolve()
    ledger = args.ledger or repo_root / "docs/status/sequencer-exposure.json"
    schema_path = args.schema or repo_root / "docs/status/sequencer-exposure.schema.json"
    try:
        document = _load_json(ledger)
    except (OSError, json.JSONDecodeError) as error:
        print(f"sequencer exposure check: cannot read {ledger}: {error}", file=sys.stderr)
        return 1
    try:
        schema = _load_json(schema_path)
    except (OSError, json.JSONDecodeError) as error:
        print(f"sequencer exposure check: cannot read {schema_path}: {error}", file=sys.stderr)
        return 1
    errors = validate_document(document, repo_root)
    errors.extend(validate_schema_contract(schema))
    errors.extend(validate_git_provenance(document, repo_root))
    errors.extend(validate_tombstone_provenance(document, repo_root))
    errors.extend(validate_release_evidence(document, repo_root))
    if args.base:
        base_document, changed_paths, transition_errors = _load_base_transition(
            repo_root, args.base
        )
        trailer_ids, trailer_errors = _load_sequencer_trailers(repo_root, args.base)
        semantic_paths, semantic_errors = _semantic_added_paths(
            repo_root, args.base, changed_paths
        )
        errors.extend(transition_errors)
        errors.extend(trailer_errors)
        errors.extend(semantic_errors)
        errors.extend(
            validate_transition(
                base_document,
                document,
                changed_paths,
                trailer_ids,
                semantic_paths,
            )
        )
    if errors:
        print("sequencer exposure check: FAILED", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"sequencer exposure check: OK ({len(document['rows'])} rows, "
          f"{len(document['tombstones'])} tombstones)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
