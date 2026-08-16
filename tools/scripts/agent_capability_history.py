"""Append-only history and protected-base checks for agent capabilities."""
from __future__ import annotations

import copy
import json
import os
import pathlib
import re
import subprocess
from typing import Any

import agent_capability_surface as surface
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
) -> list[str]:
    """Compare against protected-tip artifacts, which this checkout cannot edit."""
    base_ref = os.environ.get("PULP_AGENT_CAPABILITY_BASE_REF", "origin/main")
    if _git_output(root, ["rev-parse", "--is-inside-work-tree"]) != "true":
        # Source archives have no independently addressable protected history.
        # Their self-contained history is still checked above; PR/CI checkouts
        # must resolve or fetch the immutable protected tip below.
        return []
    protected_tip = _resolve_protected_tip(root, base_ref)
    if protected_tip is None:
        return [
            f"could not resolve protected capability history base {base_ref!r}; "
            "set PULP_AGENT_CAPABILITY_BASE_REF to the CI base ref"
        ]
    tip_manifest = _git_json(root, protected_tip, SNAPSHOT)
    old_manifest = tip_manifest
    old_surface = _git_json(root, protected_tip, surface.SURFACE_SNAPSHOT)
    old_history = _git_json(root, protected_tip, HISTORY_FILE)
    if old_manifest is None and old_surface is None and old_history is None:
        if len(history.get("entries", [])) != 1:
            return ["initial capability history bootstrap must contain exactly one entry"]
        return []
    problems: list[str] = []
    if old_manifest is None or old_surface is None or old_history is None:
        return ["protected base has an incomplete capability history contract"]
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
    explicit_ref = "PULP_AGENT_CAPABILITY_BASE_REF" in os.environ
    if explicit_ref:
        tip = _git_output(
            root, ["rev-parse", "--verify", f"{base_ref}^{{commit}}"]
        )
        if tip is not None:
            return tip
    candidate: str | None = None
    event_path = (
        os.environ.get("GITHUB_EVENT_PATH")
        if os.environ.get("GITHUB_ACTIONS") == "true"
        else None
    )
    if event_path is not None:
        try:
            event = json.loads(pathlib.Path(event_path).read_text())
            candidate = event.get("pull_request", {}).get("base", {}).get("sha")
            candidate = candidate or event.get("merge_group", {}).get("base_sha")
            before = event.get("before")
            if (
                candidate is None
                and isinstance(before, str)
                and before != "0" * 40
            ):
                candidate = before
        except (OSError, json.JSONDecodeError, AttributeError):
            candidate = None
    if isinstance(candidate, str) and re.fullmatch(r"[0-9a-fA-F]{40}", candidate):
        if _git_output(root, ["cat-file", "-e", f"{candidate}^{{commit}}"]) is None:
            try:
                fetched = subprocess.run(
                    ["git", "fetch", "--no-tags", "--depth=1", "origin", candidate],
                    cwd=root,
                    text=True,
                    capture_output=True,
                )
            except OSError:
                return None
            if fetched.returncode != 0:
                return None
        return candidate.lower()
    return _resolve_local_base(root, base_ref)

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
    Falls back to the raw tip when there is no merge-base (unrelated histories,
    or a shallow clone whose graph cannot answer), preserving prior behaviour
    rather than failing closed on a checkout the check simply cannot reason about.
    """
    tip = _git_output(root, ["rev-parse", "--verify", f"{base_ref}^{{commit}}"])
    if tip is None:
        return None
    head = _git_output(root, ["rev-parse", "--verify", "HEAD^{commit}"])
    if head is None:
        return tip
    if _git_succeeds(root, ["merge-base", "--is-ancestor", tip, head]):
        return tip
    return _git_output(root, ["merge-base", head, tip]) or tip

def _git_succeeds(root: pathlib.Path, arguments: list[str]) -> bool:
    try:
        result = subprocess.run(
            ["git", *arguments], cwd=root, text=True, capture_output=True
        )
    except OSError:
        return False
    return result.returncode == 0
