#!/usr/bin/env python3
"""Enforce Pulp's durable source-authority boundary with Vellum."""

from __future__ import annotations

import argparse
import base64
import collections
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any, Iterable


ROOT = pathlib.Path(__file__).resolve().parents[2]
MAP_PATH = ".github/vellum-ownership.json"
MANIFEST_PATH = "docs/contracts/vellum-initial-cut-manifest.json"
EVENT_PREFIX = ".github/vellum-change-events/"
EXPECTED_FRAMEWORK_REPOSITORY = "Generous-Corp/vellum"
EXPECTED_FREEZE_OWNER = "@danielraffel"
EXPECTED_SPEC_COMMIT = "a97eec582df79c22ed43968e20b36ac92d866129"
EXPECTED_SPEC_BLOB = "32daed709c00fa754e7e0cd31e16c704f6e04507"
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
EVENT_ID_RE = re.compile(r"^[0-9]{8}-[a-z0-9][a-z0-9-]{2,79}$")
SLICE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{1,79}$")
ISSUE_RE = re.compile(
    r"^https://github\.com/Generous-Corp/(?:pulp|vellum)/issues/[1-9][0-9]*$"
)
MAX_EVENT_BYTES = 32 * 1024
MAX_EVENTS_PER_CHANGE = 20
MAX_OUTBOX_BYTES = 48 * 1024
MAX_EMERGENCY_DAYS = 14
CHANGE_DISPOSITIONS = {
    "pulp-only",
    "framework-backport",
    "emergency-exception",
}


class FreezeError(RuntimeError):
    pass


@dataclass(frozen=True)
class DiffEntry:
    status: str
    old_path: str | None
    new_path: str | None

    @property
    def paths(self) -> tuple[str, ...]:
        return tuple(path for path in (self.old_path, self.new_path) if path)


def _git(repo: pathlib.Path, *args: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise FreezeError(f"git {' '.join(args)} failed: {detail}")
    return result.stdout


def _decode_path(value: bytes) -> str:
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise FreezeError("Vellum-owned paths must be valid UTF-8") from exc


def changed_entries(base: str, head: str, repo: pathlib.Path = ROOT) -> list[DiffEntry]:
    raw = _git(repo, "diff", "--name-status", "-z", "-M", "--no-ext-diff", base, head)
    fields = raw.split(b"\0")
    if fields and fields[-1] == b"":
        fields.pop()
    entries: list[DiffEntry] = []
    index = 0
    while index < len(fields):
        status = fields[index].decode("ascii", errors="strict")
        index += 1
        if status.startswith(("R", "C")):
            if index + 1 >= len(fields):
                raise FreezeError("truncated rename/copy record in git diff")
            old_path = _decode_path(fields[index])
            new_path = _decode_path(fields[index + 1])
            index += 2
        else:
            if index >= len(fields):
                raise FreezeError("truncated path record in git diff")
            path = _decode_path(fields[index])
            index += 1
            old_path = None if status.startswith("A") else path
            new_path = None if status.startswith("D") else path
        entries.append(DiffEntry(status=status, old_path=old_path, new_path=new_path))
    return entries


def _json_at(
    repo: pathlib.Path,
    revision: str,
    path: str,
    *,
    max_bytes: int | None = None,
) -> dict[str, Any] | None:
    result = subprocess.run(
        ["git", "-C", str(repo), "show", f"{revision}:{path}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        return None
    if max_bytes is not None and len(result.stdout) > max_bytes:
        raise FreezeError(f"{revision}:{path} exceeds {max_bytes} bytes")
    try:
        value = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise FreezeError(f"{revision}:{path} is invalid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise FreezeError(f"{revision}:{path} must contain a JSON object")
    return value


def _parse_utc(value: Any, field: str) -> dt.datetime:
    if not isinstance(value, str) or not value.endswith("Z"):
        raise FreezeError(f"{field} must be an ISO-8601 UTC timestamp ending in Z")
    try:
        parsed = dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as exc:
        raise FreezeError(f"{field} is not a valid ISO-8601 timestamp") from exc
    return parsed


def validate_map(mapping: dict[str, Any]) -> None:
    allowed_top = {
        "schema_version",
        "framework_repository",
        "freeze_owner",
        "activation",
        "slices",
    }
    if set(mapping) != allowed_top:
        raise FreezeError("ownership map has missing or unknown top-level fields")
    if mapping.get("schema_version") != 1:
        raise FreezeError("ownership map schema_version must be 1")
    if mapping.get("framework_repository") != EXPECTED_FRAMEWORK_REPOSITORY:
        raise FreezeError("ownership map framework_repository is not the approved repository")
    if mapping.get("freeze_owner") != EXPECTED_FREEZE_OWNER:
        raise FreezeError("ownership map freeze_owner is not the approved owner")

    activation = mapping.get("activation")
    if not isinstance(activation, dict):
        raise FreezeError("ownership map activation must be an object")
    allowed_activation = {
        "state",
        "pulp_extraction_base",
        "vellum_authority_commit",
        "accepted_by",
        "accepted_at",
    }
    if set(activation) != allowed_activation:
        raise FreezeError("activation has missing or unknown fields")
    state = activation.get("state")
    if state not in {"prepared", "active"}:
        raise FreezeError("activation.state must be prepared or active")
    base = activation.get("pulp_extraction_base")
    if not isinstance(base, str) or not SHA_RE.fullmatch(base):
        raise FreezeError("activation.pulp_extraction_base must be a 40-character SHA")
    if state == "prepared":
        if any(activation.get(field) is not None for field in (
            "vellum_authority_commit", "accepted_by", "accepted_at"
        )):
            raise FreezeError("prepared activation fields must remain null")
    else:
        authority = activation.get("vellum_authority_commit")
        if not isinstance(authority, str) or not SHA_RE.fullmatch(authority):
            raise FreezeError("active ownership requires vellum_authority_commit")
        if activation.get("accepted_by") != EXPECTED_FREEZE_OWNER:
            raise FreezeError("active ownership requires the approved freeze owner")
        accepted_at = _parse_utc(
            activation.get("accepted_at"), "activation.accepted_at"
        )
        if accepted_at > dt.datetime.now(dt.timezone.utc) + dt.timedelta(minutes=5):
            raise FreezeError("activation.accepted_at cannot be in the future")

    slices = mapping.get("slices")
    if not isinstance(slices, list):
        raise FreezeError("ownership map slices must be an array")
    seen: set[str] = set()
    for item in slices:
        if not isinstance(item, dict) or set(item) != {"id", "state", "paths"}:
            raise FreezeError("each ownership slice needs exactly id, state, and paths")
        slice_id = item.get("id")
        if not isinstance(slice_id, str) or not SLICE_ID_RE.fullmatch(slice_id):
            raise FreezeError("each ownership slice needs a lowercase kebab-case id")
        if slice_id in seen:
            raise FreezeError(f"duplicate ownership slice: {slice_id}")
        seen.add(slice_id)
        if item.get("state") not in {
            "pulp-authoritative-untransferred",
            "framework-authoritative-transferred",
            "framework-reimplemented-no-transfer",
            "pulp-only",
            "excluded",
        }:
            raise FreezeError(f"invalid state for ownership slice {slice_id}")
        paths = item.get("paths")
        if not isinstance(paths, list) or not paths or paths != sorted(set(paths)):
            raise FreezeError(f"slice {slice_id} paths must be a sorted unique array")
        for pattern in paths:
            if (
                not isinstance(pattern, str)
                or not pattern
                or pattern.startswith(("/", "-"))
                or "\\" in pattern
                or ".." in pathlib.PurePosixPath(pattern).parts
            ):
                raise FreezeError(f"slice {slice_id} has an unsafe path pattern")


def _slice_map(mapping: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {item["id"]: item for item in mapping["slices"]}


def validate_map_transition(
    base_map: dict[str, Any] | None, head_map: dict[str, Any]
) -> set[str]:
    validate_map(head_map)
    if base_map is None:
        if head_map["activation"]["state"] != "prepared":
            raise FreezeError("the first ownership projection must be prepared")
        return set()
    validate_map(base_map)
    for field in ("framework_repository", "freeze_owner"):
        if base_map[field] != head_map[field]:
            raise FreezeError(f"ownership map {field} is immutable")
    if (
        base_map["activation"]["pulp_extraction_base"]
        != head_map["activation"]["pulp_extraction_base"]
    ):
        raise FreezeError("pulp_extraction_base is immutable")
    if base_map["activation"]["state"] == "active":
        if head_map["activation"] != base_map["activation"]:
            raise FreezeError("active authority metadata is immutable")

    base_slices = _slice_map(base_map)
    head_slices = _slice_map(head_map)
    transferred_before = {
        key for key, item in base_slices.items()
        if item["state"] == "framework-authoritative-transferred"
    }
    for slice_id in transferred_before:
        if slice_id not in head_slices or head_slices[slice_id] != base_slices[slice_id]:
            raise FreezeError(
                f"transferred slice {slice_id} cannot change without a designed ownership reversal"
            )
    transferred_after = {
        key for key, item in head_slices.items()
        if item["state"] == "framework-authoritative-transferred"
    }
    added = transferred_after - transferred_before
    if added and head_map["activation"]["state"] != "active":
        raise FreezeError("transferred slices require active framework authority")
    if (
        base_map["activation"]["state"] == "prepared"
        and head_map["activation"]["state"] == "active"
        and not added
    ):
        raise FreezeError("activation must transfer at least one coherent slice")
    return added


def expected_authority_transition(
    base_map: dict[str, Any] | None, transferred: set[str]
) -> str | None:
    if not transferred:
        return None
    if base_map is None or base_map["activation"]["state"] == "prepared":
        return "activate"
    return "transfer-slice"


def validate_transferred_projection(
    mapping: dict[str, Any], manifest: dict[str, Any]
) -> None:
    entries = manifest.get("entries")
    if manifest.get("source_commit") != mapping["activation"]["pulp_extraction_base"]:
        raise FreezeError("cut manifest and ownership projection use different Pulp bases")
    if not isinstance(entries, list):
        raise FreezeError("cut manifest entries must be an array")
    manifest_by_path = {
        entry.get("source_path"): entry for entry in entries if isinstance(entry, dict)
    }
    if len(manifest_by_path) != len(entries):
        raise FreezeError("cut manifest source paths must be unique")
    # Only rows that were positively classified as framework material may cross
    # the authority boundary. ``unresolved`` is useful while preparing a cut,
    # but must never be a transfer state: those rows can contain the exact
    # product/audio assumptions this gate is intended to keep out of Vellum.
    allowed = {"framework-core", "authoring-only", "platform-adapter", "test-only"}
    owner_by_path: dict[str, str] = {}
    for item in mapping["slices"]:
        if item["state"] != "framework-authoritative-transferred":
            continue
        for path in item["paths"]:
            if path.endswith("/") or any(character in path for character in "*?["):
                raise FreezeError("transferred ownership paths must be exact manifest files")
            previous = owner_by_path.get(path)
            if previous is not None:
                raise FreezeError(
                    f"transferred path {path} overlaps slices {previous} and {item['id']}"
                )
            entry = manifest_by_path.get(path)
            if entry is None:
                raise FreezeError(f"transferred path is absent from the cut manifest: {path}")
            if entry.get("classification") not in allowed:
                raise FreezeError(
                    f"transferred path has forbidden cut classification: {path}"
                )
            owner_by_path[path] = item["id"]


def _transferred_paths_sha256(mapping: dict[str, Any]) -> str:
    value = {
        item["id"]: item["paths"]
        for item in mapping["slices"]
        if item["state"] == "framework-authoritative-transferred"
    }
    return _canonical_sha256(value)


def _matches(path: str, pattern: str) -> bool:
    if pattern.endswith("/"):
        return path.startswith(pattern)
    if any(character in pattern for character in "*?["):
        return pathlib.PurePosixPath(path).match(pattern)
    return path == pattern


def affected_slices(
    mappings: Iterable[dict[str, Any]], paths: Iterable[str]
) -> dict[str, list[str]]:
    path_list = sorted(set(paths))
    affected: dict[str, set[str]] = {}
    for mapping in mappings:
        if mapping["activation"]["state"] != "active":
            continue
        for item in mapping["slices"]:
            if item["state"] != "framework-authoritative-transferred":
                continue
            matches = {
                path for path in path_list
                if any(_matches(path, pattern) for pattern in item["paths"])
            }
            if matches:
                affected.setdefault(item["id"], set()).update(matches)
    return {key: sorted(value) for key, value in sorted(affected.items())}


def _require_strings(value: Any, field: str) -> list[str]:
    if (
        not isinstance(value, list)
        or not value
        or value != sorted(set(value))
        or not all(isinstance(item, str) and item.strip() for item in value)
    ):
        raise FreezeError(f"{field} must be a sorted, unique, non-empty string array")
    return value


def validate_event(
    event: dict[str, Any], path: str, commit_time: dt.datetime | None = None
) -> None:
    encoded = json.dumps(event, separators=(",", ":")).encode()
    if len(encoded) > MAX_EVENT_BYTES:
        raise FreezeError(f"event {path} exceeds {MAX_EVENT_BYTES} bytes")
    event_path = pathlib.PurePosixPath(path)
    event_directory = pathlib.PurePosixPath(EVENT_PREFIX.rstrip("/"))
    if event_path.parent != event_directory or event_path.suffix != ".json":
        raise FreezeError(
            f"event {path} must be a direct .json child of {EVENT_PREFIX}"
        )
    stem = event_path.stem
    if event.get("schema_version") != 1 or event.get("event_id") != stem:
        raise FreezeError(f"event {path} schema/event_id does not match its filename")
    if not EVENT_ID_RE.fullmatch(stem):
        raise FreezeError(f"event {path} has an invalid event_id")
    kind = event.get("kind")
    common = {
        "schema_version", "event_id", "kind", "created_at", "slices", "rationale", "tests"
    }
    created_at = _parse_utc(event.get("created_at"), f"event {path} created_at")
    if commit_time is not None and created_at > commit_time + dt.timedelta(minutes=5):
        raise FreezeError(f"event {path} created_at is after its source commit")
    if not isinstance(event.get("rationale"), str) or not event["rationale"].strip():
        raise FreezeError(f"event {path} requires a rationale")
    _require_strings(event.get("slices"), f"event {path} slices")
    _require_strings(event.get("tests"), f"event {path} tests")

    if kind == "change":
        disposition = event.get("disposition")
        allowed = set(common) | {"disposition"}
        if disposition == "framework-backport":
            allowed.add("framework_commit")
            commit = event.get("framework_commit")
            if not isinstance(commit, str) or not SHA_RE.fullmatch(commit):
                raise FreezeError(f"event {path} needs a 40-character framework_commit")
        elif disposition == "emergency-exception":
            allowed.update({"owner", "expiry", "follow_up"})
            owner = event.get("owner")
            if not isinstance(owner, str) or not owner.startswith("@"):
                raise FreezeError(f"event {path} emergency owner must be accountable")
            if not isinstance(event.get("follow_up"), str) or not ISSUE_RE.fullmatch(
                event["follow_up"]
            ):
                raise FreezeError(f"event {path} emergency follow_up must be an issue URL")
            try:
                expiry = dt.date.fromisoformat(event.get("expiry", ""))
            except ValueError as exc:
                raise FreezeError(f"event {path} expiry must be YYYY-MM-DD") from exc
            created_date = created_at.date()
            if (
                expiry < created_date
                or expiry > created_date + dt.timedelta(days=MAX_EMERGENCY_DAYS)
            ):
                raise FreezeError(
                    f"event {path} emergency expiry must be within {MAX_EMERGENCY_DAYS} days"
                )
            if commit_time is not None and expiry < commit_time.date():
                raise FreezeError(
                    f"event {path} emergency is already expired at its source commit"
                )
        elif disposition != "pulp-only":
            raise FreezeError(f"event {path} has an invalid change disposition")
        if set(event) != allowed:
            raise FreezeError(f"event {path} has missing or unknown fields")
    elif kind == "authority-transition":
        allowed = set(common) | {
            "transition",
            "vellum_authority_commit",
            "approved_by",
            "counterpart",
        }
        if set(event) != allowed or event.get("transition") not in {
            "activate",
            "transfer-slice",
        }:
            raise FreezeError(f"event {path} has an invalid authority transition shape")
        commit = event.get("vellum_authority_commit")
        if not isinstance(commit, str) or not SHA_RE.fullmatch(commit):
            raise FreezeError(f"event {path} needs a 40-character authority commit")
        if event.get("approved_by") != EXPECTED_FREEZE_OWNER:
            raise FreezeError(f"event {path} requires the freeze owner's approval")
        counterpart = event.get("counterpart")
        if not isinstance(counterpart, str) or not counterpart.startswith(
            "provenance/activation/"
        ):
            raise FreezeError(f"event {path} needs a Vellum activation counterpart path")
    else:
        raise FreezeError(f"event {path} has an invalid kind")


def load_new_events(
    repo: pathlib.Path, head: str, entries: list[DiffEntry]
) -> list[tuple[str, dict[str, Any]]]:
    event_changes = [
        entry for entry in entries
        if any(path.startswith(EVENT_PREFIX) for path in entry.paths)
    ]
    for entry in event_changes:
        if entry.status != "A" or entry.new_path is None:
            raise FreezeError("Vellum outbox events are append-only; modify/delete/rename is forbidden")
    if len(event_changes) > MAX_EVENTS_PER_CHANGE:
        raise FreezeError(
            f"a change may add at most {MAX_EVENTS_PER_CHANGE} Vellum events"
        )
    commit_time_raw = _git(repo, "show", "-s", "--format=%cI", head).decode().strip()
    try:
        commit_time = dt.datetime.fromisoformat(commit_time_raw)
    except ValueError as exc:
        raise FreezeError("cannot parse source commit timestamp") from exc
    loaded: list[tuple[str, dict[str, Any]]] = []
    for entry in sorted(event_changes, key=lambda item: item.new_path or ""):
        assert entry.new_path is not None
        if not entry.new_path.endswith(".json"):
            raise FreezeError("Vellum outbox contains only .json event files")
        event = _json_at(repo, head, entry.new_path, max_bytes=MAX_EVENT_BYTES)
        if event is None:
            raise FreezeError(f"cannot read added event {entry.new_path}")
        validate_event(event, entry.new_path, commit_time=commit_time)
        loaded.append((entry.new_path, event))
    return loaded


def _validate_event_coverage(
    events: list[tuple[str, dict[str, Any]]],
    affected: dict[str, list[str]],
    transferred: set[str],
    authority_commit: str | None,
    expected_transition: str | None,
) -> None:
    change_events = [event for _, event in events if event["kind"] == "change"]
    authority_events = [
        event for _, event in events if event["kind"] == "authority-transition"
    ]
    change_counts = collections.Counter(
        slice_id for event in change_events for slice_id in event["slices"]
    )
    if set(change_counts) != set(affected) or any(
        count != 1 for count in change_counts.values()
    ):
        raise FreezeError(
            "change-event slice coverage must exactly match affected transferred slices"
        )
    if affected and not change_events:
        raise FreezeError("a durable change event is required for transferred paths")
    if not affected and change_events:
        raise FreezeError("change event does not correspond to a transferred-path change")

    transfer_counts = collections.Counter(
        slice_id for event in authority_events for slice_id in event["slices"]
    )
    if set(transfer_counts) != transferred or any(
        count != 1 for count in transfer_counts.values()
    ):
        raise FreezeError("authority-event slice coverage must exactly match new transfers")
    if transferred:
        if len(authority_events) != 1:
            raise FreezeError("exactly one authority event is required for a transfer")
        if authority_events[0]["transition"] != expected_transition:
            raise FreezeError("authority event label does not match the map transition")
        if authority_events[0]["vellum_authority_commit"] != authority_commit:
            raise FreezeError("authority event does not match the ownership projection commit")
    elif authority_events:
        raise FreezeError("authority event does not correspond to a source transfer")


def build_outbox(
    *,
    base: str,
    head: str,
    source_head: str,
    affected: dict[str, list[str]],
    transferred: set[str],
    events: list[tuple[str, dict[str, Any]]],
    head_map: dict[str, Any],
) -> dict[str, Any]:
    map_bytes = (json.dumps(head_map, sort_keys=True, separators=(",", ":")) + "\n").encode()
    outbox = {
        "schema_version": 1,
        "source_repository": os.environ.get("GITHUB_REPOSITORY", "Generous-Corp/pulp"),
        "source_base": base,
        "source_commit": head,
        "source_head": source_head,
        "direction": "pulp-to-framework",
        "affected_slices": sorted(affected),
        "transferred_slices": sorted(transferred),
        "event_refs": [
            {
                "path": path,
                "sha256": hashlib.sha256(
                    json.dumps(
                        event, sort_keys=True, separators=(",", ":")
                    ).encode()
                ).hexdigest(),
            }
            for path, event in events
        ],
        "ownership_projection_sha256": hashlib.sha256(map_bytes).hexdigest(),
    }
    encoded = json.dumps(outbox, sort_keys=True, separators=(",", ":")).encode()
    if len(encoded) > MAX_OUTBOX_BYTES:
        raise FreezeError(f"compact outbox exceeds {MAX_OUTBOX_BYTES} bytes")
    return outbox


def _canonical_sha256(value: dict[str, Any]) -> str:
    encoded = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
    return hashlib.sha256(encoded).hexdigest()


def _github_json(url: str, token: str) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": "Bearer " + token,
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            value = json.load(response)
    except (urllib.error.URLError, json.JSONDecodeError) as exc:
        raise FreezeError(f"cannot verify private Vellum authority: {exc}") from exc
    if not isinstance(value, dict):
        raise FreezeError("private Vellum verification returned a non-object")
    return value


def verify_authority_counterpart(
    *,
    events: list[tuple[str, dict[str, Any]]],
    transferred: set[str],
    head_map: dict[str, Any],
    token: str | None,
) -> None:
    if not transferred:
        return
    if not token:
        raise FreezeError("VELLUM_READER_TOKEN is required for an authority transfer")
    authority_event = next(
        event for _, event in events if event["kind"] == "authority-transition"
    )
    commit = authority_event["vellum_authority_commit"]
    commit_document = _github_json(
        f"https://api.github.com/repos/{EXPECTED_FRAMEWORK_REPOSITORY}/commits/{commit}",
        token,
    )
    if commit_document.get("sha") != commit:
        raise FreezeError("Vellum authority commit did not resolve exactly")

    counterpart_path = authority_event["counterpart"]
    encoded_path = urllib.parse.quote(counterpart_path, safe="/")
    contents = _github_json(
        f"https://api.github.com/repos/{EXPECTED_FRAMEWORK_REPOSITORY}/contents/"
        f"{encoded_path}?ref={commit}",
        token,
    )
    if contents.get("encoding") != "base64" or not isinstance(contents.get("content"), str):
        raise FreezeError("Vellum authority counterpart is not a base64 file")
    try:
        encoded_content = "".join(contents["content"].split())
        counterpart = json.loads(base64.b64decode(encoded_content, validate=True))
    except (ValueError, json.JSONDecodeError) as exc:
        raise FreezeError("Vellum authority counterpart is invalid JSON") from exc
    if not isinstance(counterpart, dict):
        raise FreezeError("Vellum authority counterpart must be an object")
    allowed = {
        "schema_version",
        "state",
        "source_repository",
        "pulp_extraction_base",
        "ownership_projection_sha256",
        "transferred_paths_sha256",
        "transferred_slices",
        "burl_disposition",
        "spec_commit",
        "spec_blob",
    }
    if set(counterpart) != allowed:
        raise FreezeError("Vellum authority counterpart has missing or unknown fields")
    expected = {
        "schema_version": 1,
        "state": "pending-pulp-activation",
        "source_repository": "Generous-Corp/pulp",
        "pulp_extraction_base": head_map["activation"]["pulp_extraction_base"],
        "ownership_projection_sha256": _canonical_sha256(head_map),
        "transferred_paths_sha256": _transferred_paths_sha256(head_map),
        "transferred_slices": sorted(transferred),
        "spec_commit": EXPECTED_SPEC_COMMIT,
        "spec_blob": EXPECTED_SPEC_BLOB,
    }
    for key, expected_value in expected.items():
        if counterpart.get(key) != expected_value:
            raise FreezeError(f"Vellum authority counterpart mismatch: {key}")
    burl = counterpart.get("burl_disposition")
    if not isinstance(burl, dict) or set(burl) != {"repository", "commit", "status"}:
        raise FreezeError("Vellum authority counterpart needs an exact Burl disposition")
    if burl.get("repository") != "danielraffel/burl":
        raise FreezeError("Burl disposition names the wrong repository")
    if burl.get("status") not in {"superseded-frozen", "disjoint", "extraction-seed"}:
        raise FreezeError("Burl authority remains ambiguous")
    if not isinstance(burl.get("commit"), str) or not SHA_RE.fullmatch(burl["commit"]):
        raise FreezeError("Burl disposition needs an immutable commit")


def verify_framework_backports(
    events: list[tuple[str, dict[str, Any]]], token: str | None
) -> None:
    commits = sorted({
        event["framework_commit"]
        for _, event in events
        if event.get("kind") == "change"
        and event.get("disposition") == "framework-backport"
    })
    if commits and not token:
        raise FreezeError("VELLUM_READER_TOKEN is required for a framework backport")
    for commit in commits:
        document = _github_json(
            f"https://api.github.com/repos/{EXPECTED_FRAMEWORK_REPOSITORY}/commits/{commit}",
            token or "",
        )
        if document.get("sha") != commit:
            raise FreezeError(f"framework backport commit did not resolve: {commit}")


def _write_outputs(path: pathlib.Path, *, required: bool, output: pathlib.Path) -> None:
    payload = base64.b64encode(output.read_bytes()).decode("ascii")
    with path.open("a", encoding="utf-8") as handle:
        handle.write(f"dispatch_required={'true' if required else 'false'}\n")
        handle.write(f"outbox_path={output}\n")
        handle.write(f"outbox_b64={payload}\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=pathlib.Path, default=ROOT)
    parser.add_argument("--base", required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--source-head")
    parser.add_argument(
        "--verify-authority",
        action="store_true",
        help="verify a transfer commit and counterpart through the private Vellum API",
    )
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--github-output", type=pathlib.Path)
    args = parser.parse_args(argv)

    try:
        repo = args.repo.resolve()
        entries = changed_entries(args.base, args.head, repo)
        base_map = _json_at(repo, args.base, MAP_PATH)
        head_map = _json_at(repo, args.head, MAP_PATH)
        if head_map is None:
            raise FreezeError(f"{args.head}:{MAP_PATH} is required")
        transferred = validate_map_transition(base_map, head_map)
        manifest = _json_at(repo, args.head, MANIFEST_PATH)
        if manifest is None:
            raise FreezeError(f"{args.head}:{MANIFEST_PATH} is required")
        validate_transferred_projection(head_map, manifest)
        paths = sorted({path for entry in entries for path in entry.paths})
        affected = affected_slices(
            [mapping for mapping in (base_map, head_map) if mapping is not None], paths
        )
        events = load_new_events(repo, args.head, entries)
        authority_commit = head_map["activation"].get("vellum_authority_commit")
        _validate_event_coverage(
            events,
            affected,
            transferred,
            authority_commit,
            expected_authority_transition(base_map, transferred),
        )
        if args.verify_authority:
            token = os.environ.get("VELLUM_READER_TOKEN")
            verify_framework_backports(events, token)
            verify_authority_counterpart(
                events=events,
                transferred=transferred,
                head_map=head_map,
                token=token,
            )
        outbox = build_outbox(
            base=args.base,
            head=args.head,
            source_head=args.source_head or args.head,
            affected=affected,
            transferred=transferred,
            events=events,
            head_map=head_map,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(outbox, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        required = bool(affected or transferred)
        if args.github_output:
            _write_outputs(args.github_output, required=required, output=args.output)
    except (FreezeError, OSError) as exc:
        print(f"vellum-freeze: {exc}", file=sys.stderr)
        return 1

    if affected or transferred:
        print("Vellum authority event validated and preserved in the Pulp outbox.")
    else:
        print("No transferred Vellum slice or authority transition is affected.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
