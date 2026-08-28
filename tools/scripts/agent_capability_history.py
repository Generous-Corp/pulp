"""Append-only history and protected-base checks for agent capabilities."""
from __future__ import annotations

import copy
import json
import os
import pathlib
import re
import subprocess
from dataclasses import replace
from typing import Any

import agent_capability_surface as surface
from gate_common import (
    GitComparisonProvenance,
    git_comparison_authority_unavailable,
    git_comparison_command_failed,
    git_comparison_receipt,
    read_git_path,
    resolve_git_comparison,
)
from agent_capability_evolution import (
    manifest_evolution_problems as evolution_problems,
    surface_evolution_problems,
)

HISTORY_SCHEMA = "pulp.agent-capability-history.v1"
HISTORY_FILE = pathlib.Path("tools/agent-capabilities/contract-history.json")
SNAPSHOT = pathlib.Path("docs/status/agent-capabilities.json")


def _deduplicate(problems: list[str]) -> list[str]:
    return list(dict.fromkeys(problems))


def history_entry(
    manifest_document: dict[str, Any], surface_document: dict[str, Any]
) -> dict[str, Any]:
    material = {
        "manifest": {
            "schema": manifest_document["schema"],
            "schema_minor": manifest_document["schema_minor"],
            "manifest_revision": manifest_document["manifest_revision"],
            "required_features": copy.deepcopy(
                manifest_document["required_features"]
            ),
            "coverage": copy.deepcopy(manifest_document["coverage"]),
            "capabilities": copy.deepcopy(manifest_document["capabilities"]),
            "tombstones": copy.deepcopy(manifest_document["tombstones"]),
            "counts": copy.deepcopy(manifest_document["counts"]),
            "compatibility": copy.deepcopy(manifest_document["compatibility"]),
        },
        "surface": {
            "schema": surface_document["schema"],
            "inventory_version": surface_document["inventory_version"],
            "headers": [
                {
                    "include": row["include"],
                    "fingerprint": row["fingerprint"],
                    "disposition": row["disposition"],
                }
                for row in surface_document["headers"]
            ],
            "tombstones": copy.deepcopy(surface_document["tombstones"]),
        },
    }
    return {
        **material,
        "entry_digest": surface.canonical_digest(material),
    }

def history_document(entries: list[dict[str, Any]]) -> dict[str, Any]:
    return {"schema": HISTORY_SCHEMA, "entries": copy.deepcopy(entries)}


def updated_history_entries(
    history: Any,
    previous_manifest: Any,
    previous_surface: Any,
    current_manifest: dict[str, Any],
    current_surface: dict[str, Any],
    *,
    initial_bootstrap: bool,
    migrate_unpublished: bool,
) -> list[dict[str, Any]]:
    """Return append-only entries, replacing the sole unpublished bootstrap."""
    if initial_bootstrap or migrate_unpublished:
        return [history_entry(current_manifest, current_surface)]
    entries = (
        copy.deepcopy(history.get("entries", []))
        if isinstance(history, dict) and history.get("schema") == HISTORY_SCHEMA
        else []
    )
    if isinstance(previous_manifest, dict) and isinstance(previous_surface, dict):
        previous_entry = history_entry(previous_manifest, previous_surface)
        if not entries or entries[-1] != previous_entry:
            entries.append(previous_entry)
    if not entries:
        entries.append(history_entry(current_manifest, current_surface))
    return entries

def history_problems(
    history: Any,
    current_manifest: dict[str, Any],
    current_surface: dict[str, Any],
) -> list[str]:
    if not isinstance(history, dict):
        return ["capability history must be an object"]
    if set(history) != {"schema", "entries"}:
        return ["capability history fields must be exactly schema and entries"]
    if history.get("schema") != HISTORY_SCHEMA:
        return [f"capability history schema must be {HISTORY_SCHEMA}"]
    entries = history.get("entries")
    if not isinstance(entries, list) or not entries:
        return ["capability history must contain at least one entry"]
    problems: list[str] = []
    previous: dict[str, Any] | None = None
    for index, entry in enumerate(entries):
        where = f"capability history entries[{index}]"
        if not isinstance(entry, dict) or set(entry) != {
            "entry_digest",
            "manifest",
            "surface",
        }:
            problems.append(f"{where} fields are not exact")
            continue
        material = {"manifest": entry["manifest"], "surface": entry["surface"]}
        if entry.get("entry_digest") != surface.canonical_digest(material):
            problems.append(f"{where} digest does not match its material")
        if previous is not None:
            problems.extend(
                evolution_problems(
                    previous["manifest"],
                    entry["manifest"],
                    allow_unpublished_migration=False,
                )
            )
            problems.extend(
                surface_evolution_problems(
                    previous["surface"], entry["surface"], surface.SURFACE_SCHEMA
                )
            )
        previous = entry
    current_entry = history_entry(current_manifest, current_surface)
    if previous is not None:
        problems.extend(
            evolution_problems(
                previous["manifest"],
                current_entry["manifest"],
                allow_unpublished_migration=False,
            )
        )
        problems.extend(
            surface_evolution_problems(
                previous["surface"], current_entry["surface"], surface.SURFACE_SCHEMA
            )
        )
    return _deduplicate(problems)

def protected_base_problems(
    root: pathlib.Path,
    history: dict[str, Any],
    current_manifest: dict[str, Any],
    current_surface: dict[str, Any],
    comparison: GitComparisonProvenance | None = None,
) -> list[str]:
    """Compare against protected-tip artifacts, which this checkout cannot edit."""
    base_ref = os.environ.get("PULP_AGENT_CAPABILITY_BASE_REF", "origin/main")
    if comparison is None:
        # Source archives have no independently addressable protected history.
        # Their self-contained history is still checked above; PR/CI checkouts
        # with Git metadata must resolve or fetch the immutable tip below.
        if not (root / ".git").exists():
            return []
        comparison = _resolve_protected_comparison(root, base_ref)
    if comparison.status != "available":
        return [
            "protected capability history is unavailable; comparison receipt="
            + git_comparison_receipt(comparison)
        ]
    reads = [
        read_git_path(root, comparison, SNAPSHOT),
        read_git_path(root, comparison, surface.SURFACE_SNAPSHOT),
        read_git_path(root, comparison, HISTORY_FILE),
    ]
    unavailable = [item for item in reads if item.status not in {"available", "path_absent"}]
    if unavailable:
        return [
            "protected capability history is unavailable; comparison receipt="
            + git_comparison_receipt(unavailable[0])
        ]
    if all(item.status == "path_absent" for item in reads):
        if len(history.get("entries", [])) != 1:
            return ["initial capability history bootstrap must contain exactly one entry"]
        return []
    if any(item.status == "path_absent" for item in reads):
        return ["protected base has an incomplete capability history contract"]
    parsed: list[Any] = []
    for item in reads:
        try:
            parsed.append(json.loads(item.content or ""))
        except json.JSONDecodeError as error:
            return [
                f"protected capability authority is invalid JSON at "
                f"{item.path}: {error}"
            ]
    old_manifest, old_surface, old_history = parsed
    problems: list[str] = []
    problems.extend(append_only_history_problems(old_history, history))
    problems.extend(
        evolution_problems(
            old_manifest,
            current_manifest,
            allow_unpublished_migration=False,
        )
    )
    problems.extend(
        surface_evolution_problems(
            old_surface, current_surface, surface.SURFACE_SCHEMA
        )
    )
    return _deduplicate(problems)

def append_only_history_problems(previous: Any, current: Any) -> list[str]:
    old_entries = previous.get("entries") if isinstance(previous, dict) else None
    new_entries = current.get("entries") if isinstance(current, dict) else None
    if not isinstance(old_entries, list) or not isinstance(new_entries, list):
        return ["protected capability history entries are invalid"]
    if new_entries[: len(old_entries)] != old_entries:
        return ["capability history is not append-only relative to the protected base"]
    return []

def _git_output(root: pathlib.Path, arguments: list[str]) -> str | None:
    try:
        result = subprocess.run(
            ["git", *arguments], cwd=root, text=True, capture_output=True
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()

def _git_json(root: pathlib.Path, revision: str, path: pathlib.Path) -> Any:
    output = _git_output(root, ["show", f"{revision}:{path.as_posix()}"])
    if output is None:
        return None
    try:
        return json.loads(output)
    except json.JSONDecodeError:
        return {"_invalid": True}

def _resolve_protected_tip(root: pathlib.Path, base_ref: str) -> str | None:
    comparison = _resolve_protected_comparison(root, base_ref)
    return comparison.comparison_anchor if comparison.status == "available" else None


def _use_exact_base_authority(
    comparison: GitComparisonProvenance,
) -> GitComparisonProvenance:
    """Anchor a protected-content comparison to its exact resolved base tip.

    Capability evolution reads the protected document; it does not compute a
    branch diff. Substituting a merge-base would therefore omit protected
    changes made after the branch point and turn stale history into authority.
    """
    if (
        comparison.status == "command_failed"
        or comparison.resolved_base_tip is None
        or comparison.resolved_head is None
    ):
        return comparison
    return replace(
        comparison,
        comparison_anchor=comparison.resolved_base_tip,
        comparison_mode="exact_base",
        status="available",
        stderr="",
    )


def _resolve_protected_comparison(
    root: pathlib.Path, base_ref: str
) -> GitComparisonProvenance:
    explicit_ref = "PULP_AGENT_CAPABILITY_BASE_REF" in os.environ
    if explicit_ref:
        return _use_exact_base_authority(
            resolve_git_comparison(
                root, base_ref, source="environment_base_ref"
            )
        )
    candidate: str | None = None
    event_path = (
        os.environ.get("GITHUB_EVENT_PATH")
        if os.environ.get("GITHUB_ACTIONS") == "true"
        else None
    )
    if event_path is not None:
        try:
            event = json.loads(pathlib.Path(event_path).read_text())
            if not isinstance(event, dict):
                raise TypeError("GitHub event root is not an object")
            pull_request = event.get("pull_request")
            if "pull_request" in event and not isinstance(pull_request, dict):
                raise TypeError("GitHub event pull_request is not an object")
            pull_base = pull_request.get("base") if pull_request else None
            if pull_request and "base" in pull_request and not isinstance(pull_base, dict):
                raise TypeError("GitHub event pull_request.base is not an object")
            merge_group = event.get("merge_group")
            if "merge_group" in event and not isinstance(merge_group, dict):
                raise TypeError("GitHub event merge_group is not an object")
            candidate = pull_base.get("sha") if pull_base else None
            candidate = candidate or (
                merge_group.get("base_sha") if merge_group else None
            )
            before = event.get("before")
            if (
                candidate is None
                and isinstance(before, str)
                and before != "0" * 40
            ):
                candidate = before
        except (OSError, json.JSONDecodeError, TypeError) as error:
            resolved = resolve_git_comparison(root, base_ref, source="github_event")
            return git_comparison_authority_unavailable(
                resolved, f"cannot read GitHub event authority: {error}"
            )
        if candidate is not None and not (
            isinstance(candidate, str)
            and re.fullmatch(r"[0-9a-fA-F]{40}", candidate)
        ):
            resolved = resolve_git_comparison(root, base_ref, source="github_event")
            return git_comparison_authority_unavailable(
                resolved, "GitHub event base authority is not a 40-hex commit"
            )
    if isinstance(candidate, str) and re.fullmatch(r"[0-9a-fA-F]{40}", candidate):
        candidate = candidate.lower()
        comparison = _use_exact_base_authority(
            resolve_git_comparison(root, candidate, source="github_event")
        )
        if comparison.status == "available":
            return comparison
        if (
            comparison.status != "history_unavailable"
            or comparison.resolved_head is None
        ):
            return comparison
        if comparison.resolved_base_tip is None:
            try:
                fetched = subprocess.run(
                    ["git", "fetch", "--no-tags", "--depth=1", "origin", candidate],
                    cwd=root,
                    text=True,
                    capture_output=True,
                )
            except OSError as error:
                return git_comparison_command_failed(
                    comparison, f"could not execute git fetch: {error}"
                )
            if fetched.returncode != 0:
                return git_comparison_authority_unavailable(
                    comparison, fetched.stderr or "git fetch failed"
                )
            comparison = _use_exact_base_authority(
                resolve_git_comparison(
                    root, candidate, source="github_event"
                )
            )
            if comparison.status == "available":
                return comparison
            if (
                comparison.status != "history_unavailable"
                or comparison.resolved_head is None
                or comparison.resolved_base_tip is None
            ):
                return comparison

        return comparison
    comparison = resolve_git_comparison(root, base_ref, source="local_ref")
    if (
        event_path is not None
        and os.environ.get("GITHUB_EVENT_NAME") == "workflow_dispatch"
    ):
        return _use_exact_base_authority(comparison)
    return comparison

def _resolve_local_base(root: pathlib.Path, base_ref: str) -> str | None:
    """Resolve the protected base for a checkout with no CI event to read.

    The protected base must be an **ancestor** of the commit under validation,
    or "append-only relative to the base" is ill-posed: relative to a tip that
    carries commits this checkout does not have, every correct branch looks
    non-append-only.

    Naming a moving ref makes that failure routine rather than exotic. This
    fleet runs ~134 worktrees off one shared `.git`, so a `git fetch` in any
    sibling advances `origin/main` for all of them — including mid-validation,
    with the validating session issuing no fetch of its own. A run pinned to an
    exact commit then gets a base that moves underneath it and reports STALE
    against a tree it never examined.

    So resolve the ref, and if it is not an ancestor of HEAD, step back to the
    merge-base — the newest commit the two genuinely share. That point does not
    move when the tip advances, which is what makes the answer reproducible.
    If no merge-base is available (unrelated histories or an incomplete shallow
    graph), resolution fails closed. Comparing against the raw moving tip would
    silently answer a different question.
    """
    comparison = resolve_git_comparison(root, base_ref, source="local_ref")
    return comparison.comparison_anchor if comparison.status == "available" else None

def _git_succeeds(root: pathlib.Path, arguments: list[str]) -> bool:
    try:
        result = subprocess.run(
            ["git", *arguments], cwd=root, text=True, capture_output=True
        )
    except OSError:
        return False
    return result.returncode == 0
