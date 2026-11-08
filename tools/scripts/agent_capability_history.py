"""Append-only history and protected-base checks for agent capabilities."""
from __future__ import annotations

import copy
import hashlib
import json
import os
import pathlib
import re
import subprocess
from collections.abc import Callable
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

HISTORY_SCHEMA = "pulp.agent-capability-history.v2"
# The original layout repeated every entry's whole material inline. It is still
# read (a protected base may hold it, and --write converts it), never written.
LEGACY_HISTORY_SCHEMA = "pulp.agent-capability-history.v1"
HISTORY_FILE = pathlib.Path("tools/agent-capabilities/contract-history.json")
SNAPSHOT = pathlib.Path("docs/status/agent-capabilities.json")
# GitHub refuses any pushed file over 100 MB. The guard sits far below that so
# growth is noticed years before a push can fail.
HISTORY_SIZE_LIMIT_BYTES = 25 * 1024 * 1024

# Stored-object layout (schema v2). Every entry's material is kept in full, but
# a JSON object or array that recurs across entries is stored once in
# "objects", keyed by the canonical digest of its fully expanded value, and
# referenced as {"$ref": key}. Long arrays and objects are split into chunks at
# content-defined boundaries, {"$concat": [keys]} and {"$merge": [keys]}, so a
# one-row change stores one new chunk rather than the whole array again. The
# digest of an entry's expanded material is its entry_digest, which is also the
# key of its root object.
_REF, _CONCAT, _MERGE = "$ref", "$concat", "$merge"
_CHUNK_AVERAGE = 16
_INLINE_LIMIT = 128


class HistoryFormatError(ValueError):
    """The history document cannot be expanded into entries."""


def _deduplicate(problems: list[str]) -> list[str]:
    return list(dict.fromkeys(problems))


class _Encoder:
    """Deterministically encodes expanded entries into the stored-object layout.

    The output is a pure function of the entries, so decoding a stored history
    and encoding it again must reproduce the stored document exactly."""

    def __init__(self) -> None:
        self.objects: dict[str, Any] = {}
        # id() memo; the value pins the object so the id cannot be reused.
        self._canonical: dict[int, tuple[Any, str]] = {}

    def canonical(self, value: Any) -> str:
        """The same text surface.canonical_digest hashes, built bottom-up."""
        if isinstance(value, dict):
            cached = self._canonical.get(id(value))
            if cached is not None:
                return cached[1]
            for key in value:
                if not isinstance(key, str):
                    raise HistoryFormatError("history material has a non-string key")
                if key.startswith("$"):
                    raise HistoryFormatError(
                        f"history material key {key!r} uses the reserved '$' prefix")
            text = "{" + ",".join(
                json.dumps(key) + ":" + self.canonical(value[key])
                for key in sorted(value)
            ) + "}"
        elif isinstance(value, list):
            cached = self._canonical.get(id(value))
            if cached is not None:
                return cached[1]
            text = "[" + ",".join(self.canonical(item) for item in value) + "]"
        else:
            return json.dumps(value, ensure_ascii=True)
        self._canonical[id(value)] = (value, text)
        return text

    def digest(self, value: Any) -> str:
        return "sha256:" + hashlib.sha256(
            self.canonical(value).encode("utf-8")).hexdigest()

    @staticmethod
    def _boundary(text: str) -> bool:
        return int(hashlib.sha256(text.encode("utf-8")).hexdigest()[:8], 16) \
            % _CHUNK_AVERAGE == 0

    def _store(self, value: Any, build: Callable[[], Any]) -> str:
        key = self.digest(value)
        if key not in self.objects:
            self.objects[key] = build()
        return key

    def _chunks(self, items: list[Any], boundary_text: Callable[[Any], str]) -> list[list[Any]]:
        chunks: list[list[Any]] = [[]]
        for item in items:
            chunks[-1].append(item)
            if self._boundary(boundary_text(item)):
                chunks.append([])
        return [chunk for chunk in chunks if chunk]

    def node(self, value: Any) -> Any:
        if isinstance(value, dict):
            if len(value) > 2 * _CHUNK_AVERAGE:
                # Boundaries follow keys, so editing a value never moves them.
                return {_MERGE: [
                    self._store(
                        {key: value[key] for key in chunk},
                        lambda chunk=chunk: {key: self.child(value[key]) for key in chunk},
                    )
                    for chunk in self._chunks(sorted(value), json.dumps)
                ]}
            return {key: self.child(item) for key, item in value.items()}
        if isinstance(value, list):
            if len(value) > 2 * _CHUNK_AVERAGE:
                return {_CONCAT: [
                    self._store(chunk, lambda chunk=chunk: [self.child(item) for item in chunk])
                    for chunk in self._chunks(value, self.canonical)
                ]}
            return [self.child(item) for item in value]
        return value

    def child(self, value: Any) -> Any:
        if isinstance(value, (dict, list)) and len(self.canonical(value)) >= _INLINE_LIMIT:
            return {_REF: self._store(value, lambda: self.node(value))}
        return self.node(value)

    def entry(self, entry: Any) -> str:
        if not isinstance(entry, dict) or set(entry) != {"entry_digest", "manifest", "surface"}:
            raise HistoryFormatError("history entry fields are not exact")
        material = {"manifest": entry["manifest"], "surface": entry["surface"]}
        return self._store(material, lambda: self.node(material))


class _Decoder:
    def __init__(self, objects: dict[str, Any]) -> None:
        self.objects = objects
        self.used: set[str] = set()
        self._resolved: dict[str, Any] = {}
        self._active: set[str] = set()

    def resolve(self, key: Any, kind: type | None = None) -> Any:
        if not isinstance(key, str) or key not in self.objects:
            raise HistoryFormatError(f"capability history references a missing object {key!r}")
        if key not in self._resolved:
            if key in self._active:
                raise HistoryFormatError(f"capability history object {key} refers to itself")
            self._active.add(key)
            self._resolved[key] = self.expand(self.objects[key])
            self._active.discard(key)
            self.used.add(key)
        value = self._resolved[key]
        if kind is not None and not isinstance(value, kind):
            raise HistoryFormatError(
                f"capability history object {key} is not a {kind.__name__} chunk")
        return value

    def expand(self, node: Any) -> Any:
        if isinstance(node, list):
            return [self.expand(item) for item in node]
        if not isinstance(node, dict):
            return node
        reserved = [key for key in node if key.startswith("$")]
        if not reserved:
            return {key: self.expand(item) for key, item in node.items()}
        if len(node) != 1:
            raise HistoryFormatError("capability history reference has extra fields")
        marker, keys = next(iter(node.items()))
        if marker == _REF:
            value = self.resolve(keys)
            if not isinstance(value, (dict, list)):
                raise HistoryFormatError("capability history reference names a scalar")
            return value
        if marker not in {_CONCAT, _MERGE} or not isinstance(keys, list) or not keys:
            raise HistoryFormatError(f"capability history has an unknown marker {marker!r}")
        if marker == _CONCAT:
            joined: list[Any] = []
            for key in keys:
                joined.extend(self.resolve(key, list))
            return joined
        merged: dict[str, Any] = {}
        for key in keys:
            part = self.resolve(key, dict)
            if set(part) & set(merged):
                raise HistoryFormatError("capability history merge chunks overlap")
            merged.update(part)
        return merged


def history_entries(history: Any) -> list[dict[str, Any]]:
    """Every recorded entry, fully expanded, from either schema.

    Each entry is {"manifest", "surface", "entry_digest"} exactly as schema v1
    stored it inline. Expanded entries share unchanged sub-objects, so callers
    must not mutate them in place. Raises HistoryFormatError."""
    if not isinstance(history, dict):
        raise HistoryFormatError("capability history must be an object")
    schema = history.get("schema")
    if schema == LEGACY_HISTORY_SCHEMA:
        entries = history.get("entries")
        if not isinstance(entries, list):
            raise HistoryFormatError("capability history entries are invalid")
        return entries
    if schema != HISTORY_SCHEMA:
        raise HistoryFormatError(f"capability history schema must be {HISTORY_SCHEMA}")
    if set(history) != {"schema", "entries", "objects"}:
        raise HistoryFormatError(
            "capability history fields must be exactly schema, entries and objects")
    digests, objects = history.get("entries"), history.get("objects")
    if not isinstance(digests, list) or not isinstance(objects, dict):
        raise HistoryFormatError("capability history entries or objects are invalid")
    decoder = _Decoder(objects)
    entries = []
    for digest in digests:
        material = decoder.resolve(digest, dict)
        if set(material) != {"manifest", "surface"}:
            raise HistoryFormatError(
                f"capability history entry {digest} material fields are not exact")
        entries.append({**material, "entry_digest": digest})
    if len(decoder.used) != len(objects):
        raise HistoryFormatError("capability history stores unreferenced objects")
    return entries


def _entries_or_none(history: Any) -> list[dict[str, Any]] | None:
    try:
        return history_entries(history)
    except HistoryFormatError:
        return None


def rendered_history(history: dict[str, Any]) -> str:
    """The checked-in text: one entry digest and one stored object per line."""
    lines = ["{", f'  "schema": {json.dumps(history["schema"])},', '  "entries": [']
    digests = history["entries"]
    lines += [f"    {json.dumps(d)}{',' if i + 1 < len(digests) else ''}"
              for i, d in enumerate(digests)]
    lines += ["  ],", '  "objects": {']
    keys = sorted(history["objects"])
    for index, key in enumerate(keys):
        body = json.dumps(history["objects"][key], ensure_ascii=False,
                          separators=(",", ":"), sort_keys=True)
        lines.append(f"    {json.dumps(key)}: {body}{',' if index + 1 < len(keys) else ''}")
    lines += ["  }", "}"]
    return "\n".join(lines) + "\n"


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

def _without(document: dict[str, Any], key: str) -> dict[str, Any]:
    return {k: v for k, v in document.items() if k != key}


def pending_counters(
    history: Any, manifest_document: Any, surface_document: Any
) -> tuple[bool, bool]:
    """(manifest pending, surface pending) for a snapshot against its history.

    History holds every state before the current snapshot. A snapshot whose
    material differs from the last recorded state under the SAME counter is
    pending: a branch changed it and left the counter for the version-at-land
    bump to assign. A snapshot whose counter moved past the last state, or
    whose material equals it, is assigned."""
    entries = _entries_or_none(history)
    if (not entries
            or not isinstance(manifest_document, dict)
            or not isinstance(surface_document, dict)):
        return False, False
    last = entries[-1]
    try:
        current = history_entry(manifest_document, surface_document)
    except (KeyError, TypeError):
        return False, False
    pending = []
    for side, counter in (("manifest", "manifest_revision"), ("surface", "inventory_version")):
        old, new = last.get(side), current[side]
        pending.append(
            isinstance(old, dict)
            and new.get(counter) == old.get(counter)
            and _without(new, counter) != _without(old, counter)
        )
    return pending[0], pending[1]


def history_document(entries: list[dict[str, Any]]) -> dict[str, Any]:
    """Encode expanded entries in the stored-object layout (schema v2)."""
    encoder = _Encoder()
    digests = [encoder.entry(entry) for entry in entries]
    return {"schema": HISTORY_SCHEMA, "entries": digests, "objects": encoder.objects}


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
    """Return append-only entries, replacing the sole unpublished bootstrap.

    The previous published state is appended only when this change moves a
    counter (manifest_revision or the surface inventory_version). A refresh
    that leaves both alone, such as a catalog binding's header fingerprint,
    appends nothing: the next counter move records the state it started from.
    """
    if initial_bootstrap or migrate_unpublished:
        return [history_entry(current_manifest, current_surface)]
    # Either schema is accepted here: a v1 history is converted, never lost.
    entries = list(_entries_or_none(history) or [])
    if isinstance(previous_manifest, dict) and isinstance(previous_surface, dict):
        counters_moved = (
            previous_manifest.get("manifest_revision")
            != current_manifest.get("manifest_revision")
            or previous_surface.get("inventory_version")
            != current_surface.get("inventory_version")
        )
        previous_entry = history_entry(previous_manifest, previous_surface)
        # A pending previous state was never assigned a counter, so it is not
        # a contract identity: recording it would put two materials under one
        # counter. The assigned state that follows records the change. A
        # refresh that moves neither counter appends nothing.
        if not entries or (counters_moved and entries[-1] != previous_entry and not any(
                pending_counters(history_document(entries), previous_manifest,
                                 previous_surface))):
            entries.append(previous_entry)
    if not entries:
        entries.append(history_entry(current_manifest, current_surface))
    return entries

def history_problems(
    history: Any,
    current_manifest: dict[str, Any],
    current_surface: dict[str, Any],
) -> list[str]:
    if isinstance(history, dict) and history.get("schema") == LEGACY_HISTORY_SCHEMA:
        return [
            f"capability history uses the retired {LEGACY_HISTORY_SCHEMA} layout; "
            "run tools/scripts/agent_capability_manifest.py --write to convert it"
        ]
    try:
        entries = history_entries(history)
    except HistoryFormatError as error:
        return [str(error)]
    if not entries:
        return ["capability history must contain at least one entry"]
    problems: list[str] = []
    # Encoding is deterministic, so a stored document that is not exactly the
    # encoding of its own entries has been edited by hand: an object whose key
    # is not the digest of its content, a moved chunk boundary, or an extra or
    # missing object.
    if history_document(entries) != history:
        problems.append(
            "capability history is not in canonical stored-object form; "
            "regenerate it with tools/scripts/agent_capability_manifest.py --write"
        )
    size = len(rendered_history(history).encode("utf-8"))
    if size > HISTORY_SIZE_LIMIT_BYTES:
        problems.append(
            f"capability history renders to {size} bytes, over the "
            f"{HISTORY_SIZE_LIMIT_BYTES}-byte guard (GitHub refuses files over 100 MB)"
        )
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
        manifest_pending, surface_pending = pending_counters(
            history, current_manifest, current_surface)
        problems.extend(
            evolution_problems(
                previous["manifest"],
                current_entry["manifest"],
                allow_unpublished_migration=False,
                unassigned=manifest_pending,
            )
        )
        problems.extend(
            surface_evolution_problems(
                previous["surface"], current_entry["surface"], surface.SURFACE_SCHEMA,
                unassigned=surface_pending,
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
    manifest_pending, surface_pending = pending_counters(
        history, current_manifest, current_surface)
    base_manifest_pending, base_surface_pending = pending_counters(
        old_history, old_manifest, old_surface)
    problems.extend(
        evolution_problems(
            old_manifest,
            current_manifest,
            allow_unpublished_migration=False,
            unassigned=manifest_pending,
            assigning=base_manifest_pending and not manifest_pending,
        )
    )
    problems.extend(
        surface_evolution_problems(
            old_surface, current_surface, surface.SURFACE_SCHEMA,
            unassigned=surface_pending,
            assigning=base_surface_pending and not surface_pending,
        )
    )
    return _deduplicate(problems)

def append_only_history_problems(previous: Any, current: Any) -> list[str]:
    """Every protected entry is still recorded, unchanged and in order.

    Entries are compared fully expanded (material and digest), so the check is
    the same whichever schema each side is stored in."""
    old_entries = _entries_or_none(previous)
    new_entries = _entries_or_none(current)
    if old_entries is None or new_entries is None:
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
