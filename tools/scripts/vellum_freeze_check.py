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
EXPECTED_FRAMEWORK_REPOSITORY_ID = 1309219868
EXPECTED_FREEZE_OWNER = "@danielraffel"
EXPECTED_CHECK_APP_IDS = {
    "forbidden-deps": 15368,
    "provenance-verify": 15368,
    "sterile-consumer": 15368,
}
EXPECTED_VELLUM_READER_APP_ID = 3878000
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
EVENT_ID_RE = re.compile(r"^[0-9]{8}-[a-z0-9][a-z0-9-]{2,79}$")
SLICE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{1,79}$")
RECORD_PATH_RE = re.compile(
    r"^provenance/authority/records/[a-z0-9][a-z0-9._-]{2,120}\.json$"
)
AUTHORITY_REF_RE = re.compile(
    r"^refs/tags/authority/[a-z0-9][a-z0-9._/-]{2,120}$"
)
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
    if mapping.get("schema_version") != 2:
        raise FreezeError("ownership map schema_version must be 2")
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
        "authority_record_path",
        "initial_transition_event",
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
            "vellum_authority_commit",
            "authority_record_path",
            "initial_transition_event",
            "accepted_by",
            "accepted_at",
        )):
            raise FreezeError("prepared activation fields must remain null")
    else:
        authority = activation.get("vellum_authority_commit")
        if not isinstance(authority, str) or not SHA_RE.fullmatch(authority):
            raise FreezeError("active ownership requires vellum_authority_commit")
        record_path = activation.get("authority_record_path")
        if not isinstance(record_path, str) or not RECORD_PATH_RE.fullmatch(record_path):
            raise FreezeError("active ownership requires a direct Vellum authority record path")
        initial_event = activation.get("initial_transition_event")
        if not isinstance(initial_event, str) or not EVENT_ID_RE.fullmatch(initial_event):
            raise FreezeError("active ownership requires an initial transition event")
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
        if not isinstance(item, dict) or set(item) != {"id", "state", "paths", "authority"}:
            raise FreezeError(
                "each ownership slice needs exactly id, state, paths, and authority"
            )
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
        slice_authority = item.get("authority")
        if item.get("state") == "framework-authoritative-transferred":
            required = {
                "event_id",
                "vellum_commit",
                "counterpart",
                "accepted_by",
                "accepted_at",
            }
            if not isinstance(slice_authority, dict) or set(slice_authority) != required:
                raise FreezeError(
                    f"transferred slice {slice_id} requires exact authority metadata"
                )
            if (
                not isinstance(slice_authority.get("event_id"), str)
                or not EVENT_ID_RE.fullmatch(slice_authority["event_id"])
                or not isinstance(slice_authority.get("vellum_commit"), str)
                or not SHA_RE.fullmatch(slice_authority["vellum_commit"])
                or not isinstance(slice_authority.get("counterpart"), str)
                or not RECORD_PATH_RE.fullmatch(slice_authority["counterpart"])
                or slice_authority.get("accepted_by") != EXPECTED_FREEZE_OWNER
            ):
                raise FreezeError(f"transferred slice {slice_id} authority is invalid")
            _parse_utc(
                slice_authority.get("accepted_at"),
                f"slice {slice_id} authority.accepted_at",
            )
        elif slice_authority is not None:
            raise FreezeError(
                f"non-transferred slice {slice_id} cannot carry authority metadata"
            )


def _slice_map(mapping: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {item["id"]: item for item in mapping["slices"]}


def _upgrade_prepared_v1(mapping: dict[str, Any]) -> dict[str, Any]:
    """Normalize the one allowed schema migration without changing ownership."""

    if mapping.get("schema_version") != 1:
        return mapping
    activation = mapping.get("activation")
    slices = mapping.get("slices")
    if (
        not isinstance(activation, dict)
        or activation.get("state") != "prepared"
        or set(activation)
        != {
            "state",
            "pulp_extraction_base",
            "vellum_authority_commit",
            "accepted_by",
            "accepted_at",
        }
        or any(
            activation.get(field) is not None
            for field in ("vellum_authority_commit", "accepted_by", "accepted_at")
        )
        or not isinstance(slices, list)
        or any(
            not isinstance(item, dict)
            or set(item) != {"id", "state", "paths"}
            or item.get("state") == "framework-authoritative-transferred"
            for item in slices
        )
    ):
        raise FreezeError("only the prepared, untransferred schema-v1 map may migrate")
    normalized = json.loads(json.dumps(mapping))
    normalized["schema_version"] = 2
    normalized["activation"]["authority_record_path"] = None
    normalized["activation"]["initial_transition_event"] = None
    for item in normalized["slices"]:
        item["authority"] = None
    return normalized


def validate_map_transition(
    base_map: dict[str, Any] | None, head_map: dict[str, Any]
) -> set[str]:
    validate_map(head_map)
    if base_map is None:
        if head_map["activation"]["state"] != "prepared":
            raise FreezeError("the first ownership projection must be prepared")
        return set()
    base_map = _upgrade_prepared_v1(base_map)
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
    if added and base_map["activation"]["state"] == "active":
        raise FreezeError(
            "schema-v2 authority is a one-shot coherent transfer; later slice transfer "
            "needs a separately designed protocol"
        )
    return added


def expected_authority_transition(
    base_map: dict[str, Any] | None, transferred: set[str]
) -> str | None:
    if not transferred:
        return None
    if base_map is None or base_map["activation"]["state"] == "prepared":
        return "activate"
    raise FreezeError("schema-v2 does not support later slice transfers")


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
        }:
            raise FreezeError(f"event {path} has an invalid authority transition shape")
        commit = event.get("vellum_authority_commit")
        if not isinstance(commit, str) or not SHA_RE.fullmatch(commit):
            raise FreezeError(f"event {path} needs a 40-character authority commit")
        if event.get("approved_by") != EXPECTED_FREEZE_OWNER:
            raise FreezeError(f"event {path} requires the freeze owner's approval")
        counterpart = event.get("counterpart")
        if not isinstance(counterpart, str) or not RECORD_PATH_RE.fullmatch(counterpart):
            raise FreezeError(f"event {path} needs a direct Vellum authority record path")
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
    head_map: dict[str, Any],
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
        event = authority_events[0]
        activation = head_map["activation"]
        if (
            activation.get("initial_transition_event") != event["event_id"]
            or activation.get("authority_record_path") != event["counterpart"]
            or activation.get("accepted_by") != event["approved_by"]
            or activation.get("accepted_at") != event["created_at"]
        ):
            raise FreezeError("activation metadata does not derive from the authority event")
        for slice_id in transferred:
            item = _slice_map(head_map)[slice_id]
            if item.get("authority") != {
                "event_id": event["event_id"],
                "vellum_commit": event["vellum_authority_commit"],
                "counterpart": event["counterpart"],
                "accepted_by": event["approved_by"],
                "accepted_at": event["created_at"],
            }:
                raise FreezeError(
                    f"slice {slice_id} authority does not derive from the authority event"
                )
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


def _git_blob(repo: pathlib.Path, revision: str, path: str) -> str:
    output = _git(repo, "ls-tree", revision, "--", path).decode().strip()
    rows = output.splitlines()
    if len(rows) != 1:
        raise FreezeError(f"expected one Git blob for {revision}:{path}")
    metadata, actual_path = rows[0].split("\t", 1)
    mode, object_type, blob = metadata.split()
    if actual_path != path or object_type != "blob" or mode not in {"100644", "100755"}:
        raise FreezeError(f"expected a regular Git blob for {revision}:{path}")
    return blob


def _git_tree_entry(repo: pathlib.Path, revision: str, path: str) -> dict[str, str]:
    output = _git(repo, "ls-tree", revision, "--", path).decode().strip()
    rows = output.splitlines()
    if len(rows) != 1:
        raise FreezeError(f"expected one Git tree entry for {revision}:{path}")
    metadata, actual_path = rows[0].split("\t", 1)
    mode, object_type, blob = metadata.split()
    if actual_path != path or object_type != "blob" or mode not in {"100644", "100755"}:
        raise FreezeError(f"expected a regular Git blob for {revision}:{path}")
    return {"blob": blob, "mode": mode}


def _verify_vellum_trust(
    *,
    commit: str,
    authority_ref: str,
    token: str,
    app_jwt: str,
) -> None:
    app = _github_json("https://api.github.com/app", app_jwt)
    if app.get("id") != EXPECTED_VELLUM_READER_APP_ID:
        raise FreezeError("Vellum reader token identifies the wrong GitHub App")
    token_installation = _github_json(
        "https://api.github.com/installation", token
    )
    if token_installation.get("app_id") != EXPECTED_VELLUM_READER_APP_ID:
        raise FreezeError(
            "Vellum reader installation token belongs to the wrong GitHub App"
        )
    repository = _github_json(
        f"https://api.github.com/repos/{EXPECTED_FRAMEWORK_REPOSITORY}", token
    )
    if (
        repository.get("id") != EXPECTED_FRAMEWORK_REPOSITORY_ID
        or repository.get("full_name") != EXPECTED_FRAMEWORK_REPOSITORY
        or repository.get("private") is not True
        or repository.get("archived") is not False
    ):
        raise FreezeError("Vellum repository identity or state differs")
    installation = _github_json(
        "https://api.github.com/installation/repositories?per_page=100", token
    )
    repositories = installation.get("repositories")
    if (
        installation.get("total_count") != 1
        or not isinstance(repositories, list)
        or len(repositories) != 1
        or not isinstance(repositories[0], dict)
        or repositories[0].get("id") != EXPECTED_FRAMEWORK_REPOSITORY_ID
    ):
        raise FreezeError("Vellum reader token is not scoped to exactly one repository")
    tag_name = authority_ref.removeprefix("refs/tags/")
    encoded_tag_path = urllib.parse.quote(tag_name, safe="/")
    reference = _github_json(
        f"https://api.github.com/repos/{EXPECTED_FRAMEWORK_REPOSITORY}/git/ref/"
        f"tags/{encoded_tag_path}",
        token,
    )
    tag_object = reference.get("object")
    if (
        not isinstance(tag_object, dict)
        or tag_object.get("type") != "tag"
        or not isinstance(tag_object.get("sha"), str)
    ):
        raise FreezeError("Vellum authority ref is not an annotated tag")
    tag = _github_json(
        f"https://api.github.com/repos/{EXPECTED_FRAMEWORK_REPOSITORY}/git/tags/"
        f"{tag_object['sha']}",
        token,
    )
    target = tag.get("object")
    verification = tag.get("verification")
    if (
        not isinstance(target, dict)
        or target.get("type") != "commit"
        or target.get("sha") != commit
        or not isinstance(verification, dict)
        or verification.get("verified") is not True
        or verification.get("reason") != "valid"
    ):
        raise FreezeError(
            "Vellum authority tag is unsigned or targets the wrong commit"
        )
    release = _github_json(
        f"https://api.github.com/repos/{EXPECTED_FRAMEWORK_REPOSITORY}/"
        f"releases/tags/{urllib.parse.quote(tag_name, safe='')}",
        token,
    )
    if (
        release.get("tag_name") != tag_name
        or release.get("draft") is not False
        or release.get("immutable") is not True
        or not isinstance(release.get("published_at"), str)
    ):
        raise FreezeError(
            "Vellum authority tag lacks a published immutable release"
        )
    expected = set(EXPECTED_CHECK_APP_IDS.items())
    check_runs = _github_json(
        f"https://api.github.com/repos/{EXPECTED_FRAMEWORK_REPOSITORY}/commits/"
        f"{commit}/check-runs?per_page=100",
        token,
    ).get("check_runs")
    if not isinstance(check_runs, list):
        raise FreezeError("Vellum authority check runs are unavailable")
    successful: set[tuple[str, int]] = set()
    for run in check_runs:
        if (
            isinstance(run, dict)
            and run.get("head_sha") == commit
            and run.get("conclusion") == "success"
            and isinstance(run.get("app"), dict)
            and isinstance(run["app"].get("id"), int)
            and isinstance(run.get("name"), str)
        ):
            successful.add((run["name"], run["app"]["id"]))
    if not expected.issubset(successful):
        raise FreezeError("Vellum authority commit lacks successful pinned checks")


def verify_authority_counterpart(
    *,
    repo: pathlib.Path,
    base: str,
    head: str,
    manifest: dict[str, Any],
    events: list[tuple[str, dict[str, Any]]],
    transferred: set[str],
    head_map: dict[str, Any],
    token: str | None,
    app_jwt: str | None,
) -> None:
    if not transferred:
        return
    if not token:
        raise FreezeError("VELLUM_READER_TOKEN is required for an authority transfer")
    if not app_jwt:
        raise FreezeError(
            "VELLUM_READER_APP_JWT is required for an authority transfer"
        )
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
        "framework_repository",
        "pulp_extraction_base",
        "historical_seed_commit",
        "pulp_candidate_commit",
        "pulp_ownership_projection_blob",
        "authority_start_commit",
        "authority_record_ref",
        "cut_manifest_sha256",
        "authority_groups",
        "pulp_activation",
        "approved_by",
        "approved_at",
    }
    if set(counterpart) != allowed:
        raise FreezeError("Vellum authority counterpart has missing or unknown fields")
    expected = {
        "schema_version": 2,
        "state": "pending-pulp-activation",
        "source_repository": "Generous-Corp/pulp",
        "framework_repository": EXPECTED_FRAMEWORK_REPOSITORY,
        "pulp_extraction_base": head_map["activation"]["pulp_extraction_base"],
        "pulp_activation": None,
        "approved_by": EXPECTED_FREEZE_OWNER,
    }
    for key, expected_value in expected.items():
        if counterpart.get(key) != expected_value:
            raise FreezeError(f"Vellum authority counterpart mismatch: {key}")
    for field in (
        "historical_seed_commit",
        "pulp_candidate_commit",
        "pulp_ownership_projection_blob",
        "authority_start_commit",
    ):
        if not isinstance(counterpart.get(field), str) or not SHA_RE.fullmatch(
            counterpart[field]
        ):
            raise FreezeError(f"Vellum authority counterpart has invalid {field}")
    digest = counterpart.get("cut_manifest_sha256")
    if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
        raise FreezeError("Vellum authority counterpart has an invalid cut manifest digest")
    if digest != _canonical_sha256(manifest):
        raise FreezeError("Vellum authority counterpart cut manifest digest differs")
    candidate = counterpart["pulp_candidate_commit"]
    resolved = _git(repo, "rev-parse", f"{candidate}^{{commit}}").decode().strip()
    if resolved != candidate:
        raise FreezeError("recorded Pulp candidate commit did not resolve exactly")
    ancestor = subprocess.run(
        ["git", "-C", str(repo), "merge-base", "--is-ancestor", candidate, base],
        check=False,
    )
    if ancestor.returncode != 0:
        raise FreezeError("recorded Pulp candidate is not an ancestor of the activation base")
    if _git_blob(repo, candidate, MAP_PATH) != counterpart["pulp_ownership_projection_blob"]:
        raise FreezeError("recorded Pulp candidate ownership blob differs")
    reference = counterpart.get("authority_record_ref")
    if (
        not isinstance(reference, str)
        or not AUTHORITY_REF_RE.fullmatch(reference)
        or ".." in pathlib.PurePosixPath(reference).parts
    ):
        raise FreezeError("Vellum authority record ref is invalid")
    _verify_vellum_trust(
        commit=commit,
        authority_ref=reference,
        token=token,
        app_jwt=app_jwt,
    )
    groups = counterpart.get("authority_groups")
    if not isinstance(groups, list) or not groups:
        raise FreezeError("Vellum authority counterpart needs authority groups")
    recorded_slices: list[str] = []
    recorded_paths: dict[str, dict[str, str]] = {}
    entries = manifest.get("entries")
    if not isinstance(entries, list):
        raise FreezeError("Pulp cut manifest entries are invalid")
    manifest_by_path = {
        item.get("source_path"): item for item in entries if isinstance(item, dict)
    }
    if len(manifest_by_path) != len(entries):
        raise FreezeError("Pulp cut manifest paths are not unique")
    group_fields = {
        "id",
        "lineage_mode",
        "pulp_legacy_slices",
        "pulp_historical_seed_projection",
        "pulp_activation_candidate_projection",
        "vellum_implementation_projection",
    }
    for group in groups:
        if not isinstance(group, dict) or set(group) != group_fields:
            raise FreezeError("Vellum authority group has missing or unknown fields")
        slices = group.get("pulp_legacy_slices")
        if (
            group.get("lineage_mode")
            != "history-seed-ancestor-active-reimplementation"
            or not isinstance(slices, list)
            or not slices
            or slices != sorted(set(slices))
        ):
            raise FreezeError("Vellum authority group lineage or slices are invalid")
        for field in (
            "pulp_historical_seed_projection",
            "pulp_activation_candidate_projection",
            "vellum_implementation_projection",
        ):
            if not isinstance(group.get(field), dict) or not group[field]:
                raise FreezeError(f"Vellum authority group {field} must be non-empty")
        if set(group["pulp_historical_seed_projection"]) != set(
            group["pulp_activation_candidate_projection"]
        ):
            raise FreezeError("Vellum authority group candidate path sets differ")
        for path, metadata in group["pulp_historical_seed_projection"].items():
            entry = manifest_by_path.get(path)
            if (
                not isinstance(entry, dict)
                or not isinstance(metadata, dict)
                or set(metadata) != {"blob", "mode", "classification"}
                or metadata
                != {
                    "blob": entry.get("git_blob_sha"),
                    "mode": entry.get("git_mode"),
                    "classification": entry.get("classification"),
                }
            ):
                raise FreezeError(
                    f"Vellum authority historical projection differs at {path}"
                )
        for path, metadata in group["pulp_activation_candidate_projection"].items():
            if (
                not isinstance(path, str)
                or not isinstance(metadata, dict)
                or set(metadata) != {"blob", "mode"}
                or path in recorded_paths
            ):
                raise FreezeError("Vellum authority candidate projection is invalid")
            if _git_tree_entry(repo, candidate, path) != metadata:
                raise FreezeError(
                    f"Vellum authority candidate projection differs at {path}"
                )
            recorded_paths[path] = metadata
        recorded_slices.extend(slices)
    if len(recorded_slices) != len(set(recorded_slices)):
        raise FreezeError("Vellum authority record repeats a Pulp slice")
    if sorted(recorded_slices) != sorted(transferred):
        raise FreezeError("Vellum authority record slice set differs from the transfer")
    transferred_paths = sorted(
        path
        for item in head_map["slices"]
        if item["id"] in transferred
        for path in item["paths"]
    )
    if transferred_paths != sorted(recorded_paths):
        raise FreezeError("Vellum authority record path set differs from the transfer")
    changed = _git(
        repo,
        "diff",
        "--name-only",
        candidate,
        head,
        "--",
        *sorted(recorded_paths),
    ).decode().splitlines()
    if changed:
        raise FreezeError(
            "Pulp candidate source changed before activation: " + ", ".join(changed)
        )
    approved_at = counterpart.get("approved_at")
    _parse_utc(approved_at, "Vellum authority counterpart approved_at")


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
        # The trusted controls must land before the durable projection can
        # migrate. Normalize the one safe, prepared v1 state in memory so a
        # control-only bootstrap commit remains verifiable while the following
        # data commit performs the explicit v1 -> v2 transition.
        if base_map is not None:
            base_map = _upgrade_prepared_v1(base_map)
        head_map = _upgrade_prepared_v1(head_map)
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
            head_map,
        )
        if args.verify_authority:
            token = os.environ.get("VELLUM_READER_TOKEN")
            app_jwt = os.environ.get("VELLUM_READER_APP_JWT")
            verify_framework_backports(events, token)
            verify_authority_counterpart(
                repo=repo,
                base=args.base,
                head=args.head,
                manifest=manifest,
                events=events,
                transferred=transferred,
                head_map=head_map,
                token=token,
                app_jwt=app_jwt,
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
