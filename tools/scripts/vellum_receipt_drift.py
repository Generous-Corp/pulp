#!/usr/bin/env python3
"""Receipt mode of the Vellum freeze check: no silent drift from the last export.

Vellum's mirror is a copy of named Pulp files at one Pulp commit, recorded in
an export receipt. Until Pulp's copy of those files is frozen, a Pulp change to
one of them either reaches Vellum on the next re-export or it does not, and
nothing in the diff says which. This check makes that a recorded decision.

A pinned copy of the newest receipt lives under ``PIN_DIRECTORY``. A change
fails when it modifies, adds or deletes an exported path, changes a path a
generated export input is produced from, or adds any file under a directory
that holds exported source, unless an event added by the same change covers
the path and states ``vellum_inherits`` explicitly. Three event kinds count:

- a change event (``.github/vellum-change-events/``) covers the paths of the
  transferred slices it names;
- an expansion-watch event (``.github/vellum-expansion-watch-events/``) covers
  the paths its capability families select;
- an export event (``EXPORT_EVENT_DIRECTORY``) covers the exact paths it lists.
  It is the only vehicle for an exported path that is in neither a transferred
  slice nor a watch family.

Two events that state different values for one path conflict, and the check
fails. The one way to settle a conflict is an export event that names the
earlier export event it corrects in ``supersedes``: for the paths it lists, the
superseded event's value no longer counts and the correcting event's value
does. It may list only paths every superseded event lists, so it corrects a
recorded decision and cannot claim a path nobody decided.

Paths the pin lists as frozen owe no inheritance decision, because a frozen
path is not edited in Pulp at all: its authoritative copy is Vellum's. A change
to a frozen path fails unless a change event added by the same change covers it
through its transferred slice with disposition ``framework-backport`` (bringing
a named Vellum commit back) or ``emergency-exception``. A ``pulp-only`` change
event does not unfreeze a path.

The pin, the receipt copy and the slice map are always read at the BASE
commit, so a change cannot exempt itself by editing them; a re-pin takes effect
for the changes after it. Events are read at the head commit, since they are
the change's own statements.

``receipt_copy_problems`` proves each pinned copy is Vellum's receipt: its Git
blob id must equal the one Vellum's main serves for ``receipt_source``. It needs
a Vellum reader, so only the trusted lanes run it.
"""

from __future__ import annotations

import dataclasses
import datetime as dt
import hashlib
import json
import pathlib
import posixpath
import re
import subprocess
import sys
from typing import Any, Callable, Iterable

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import vellum_expansion_watch_check as watch  # noqa: E402  (needs the path insert)


PIN_DIRECTORY = ".github/vellum-export-receipt"
PIN_PATH = f"{PIN_DIRECTORY}/pin.json"
EXPORT_EVENT_DIRECTORY = ".github/vellum-export-events"
CHANGE_EVENT_DIRECTORY = ".github/vellum-change-events"
WATCH_EVENT_DIRECTORY = ".github/vellum-expansion-watch-events"
MAP_PATH = ".github/vellum-ownership.json"
RECEIPT_SCHEMA = "pulp.vellum.export-receipt.v2"
PULP_REPOSITORY = "Generous-Corp/pulp"
FRAMEWORK_REPOSITORY = "Generous-Corp/vellum"
EXPORT_ID_RE = re.compile(r"^[0-9]{8}-[0-9a-f]{12}$")
EVENT_ID_RE = re.compile(r"^[0-9]{8}-[a-z0-9][a-z0-9-]{2,79}$")
SHA40_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
MAX_EVENT_BYTES = 32 * 1024
# The change-event dispositions that may touch a frozen path.
FROZEN_EDIT_DISPOSITIONS = frozenset({"framework-backport", "emergency-exception"})
MAX_RECEIPT_BYTES = 8 * 1024 * 1024
# A directory holding an exported file with one of these suffixes is a source
# directory of an exported target: a file added anywhere under it is compiled
# into, generated into, or shipped beside that target.
SOURCE_SUFFIXES = frozenset({
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl",
    ".ipp", ".m", ".mm", ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx",
    ".glsl", ".wgsl", ".sksl", ".metal",
})


class ReceiptDriftError(RuntimeError):
    """The pin, receipt or an event is malformed, or the check cannot run."""


@dataclasses.dataclass(frozen=True)
class Pin:
    export_id: str
    receipt_path: str
    receipt_sha256: str
    frozen_paths: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class Receipt:
    export_id: str
    pulp_sha: str
    exported: frozenset[str]
    producers: frozenset[str]
    source_dirs: frozenset[str]


def _git(repo: pathlib.Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if check and result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise ReceiptDriftError(f"git {' '.join(args)} failed: {detail}")
    return result


def _blob(repo: pathlib.Path, commit: str, path: str, max_bytes: int) -> bytes | None:
    """The bytes of a regular file at ``commit``; None when absent."""
    listing = _git(repo, "ls-tree", "-z", commit, "--", path).stdout
    if not listing:
        return None
    meta, _, name = listing.rstrip(b"\0").partition(b"\t")
    mode, kind, _ = meta.split(b" ", 2)
    if name.decode("utf-8", errors="replace") != path or kind != b"blob":
        raise ReceiptDriftError(f"{commit[:12]}:{path} is not a file")
    if mode not in (b"100644", b"100755"):
        raise ReceiptDriftError(f"{commit[:12]}:{path} must be a regular file, not mode {mode.decode()}")
    size = int(_git(repo, "cat-file", "-s", f"{commit}:{path}").stdout.strip())
    if size > max_bytes:
        raise ReceiptDriftError(f"{commit[:12]}:{path} exceeds {max_bytes} bytes")
    return _git(repo, "cat-file", "blob", f"{commit}:{path}").stdout


def _strict_json(raw: bytes, where: str) -> Any:
    def pairs(items: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in items:
            if key in result:
                raise ReceiptDriftError(f"{where}: duplicate JSON key {key!r}")
            result[key] = value
        return result

    try:
        return json.loads(raw.decode("utf-8"), object_pairs_hook=pairs)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ReceiptDriftError(f"{where}: invalid JSON: {exc}") from exc


def _exact_keys(value: Any, keys: set[str], where: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != keys:
        actual = set(value) if isinstance(value, dict) else set()
        raise ReceiptDriftError(
            f"{where}: fields differ; missing={sorted(keys - actual)} "
            f"unexpected={sorted(actual - keys)}"
        )
    return value


def _safe_path(value: Any, where: str, *, directory_ok: bool = False) -> str:
    if not isinstance(value, str) or not value:
        raise ReceiptDriftError(f"{where}: expected a repository-relative path")
    body = value[:-1] if directory_ok and value.endswith("/") else value
    parts = body.split("/")
    if (
        value.startswith("/")
        or "\\" in value
        or any(part in {"", ".", ".."} for part in parts)
        or any(marker in value for marker in "*?[]")
    ):
        raise ReceiptDriftError(f"{where}: {value!r} is not one exact repository path")
    return value


def _sorted_unique(value: Any, where: str, *, allow_empty: bool) -> list[Any]:
    if not isinstance(value, list) or (not value and not allow_empty):
        raise ReceiptDriftError(f"{where}: expected a{'n' if allow_empty else ' non-empty'} array")
    if not all(isinstance(item, str) for item in value) or value != sorted(set(value)):
        raise ReceiptDriftError(f"{where}: entries must be sorted, unique strings")
    return value


def parse_pin(raw: bytes) -> Pin:
    value = _exact_keys(
        _strict_json(raw, PIN_PATH),
        {
            "schema_version", "framework_repository", "export_id", "receipt_source",
            "receipt_path", "receipt_sha256", "frozen_paths",
        },
        PIN_PATH,
    )
    if type(value["schema_version"]) is not int or value["schema_version"] != 1:
        raise ReceiptDriftError(f"{PIN_PATH}: schema_version must be 1")
    if value["framework_repository"] != FRAMEWORK_REPOSITORY:
        raise ReceiptDriftError(f"{PIN_PATH}: framework_repository must be {FRAMEWORK_REPOSITORY}")
    export_id = value["export_id"]
    if not isinstance(export_id, str) or not EXPORT_ID_RE.fullmatch(export_id):
        raise ReceiptDriftError(f"{PIN_PATH}: export_id is malformed")
    if value["receipt_source"] != f"provenance/mirror/receipts/{export_id}.json":
        raise ReceiptDriftError(f"{PIN_PATH}: receipt_source must name Vellum's receipt {export_id}")
    if value["receipt_path"] != f"{PIN_DIRECTORY}/{export_id}.json":
        raise ReceiptDriftError(f"{PIN_PATH}: receipt_path must be {PIN_DIRECTORY}/{export_id}.json")
    digest = value["receipt_sha256"]
    if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
        raise ReceiptDriftError(f"{PIN_PATH}: receipt_sha256 must be a sha256 hex digest")
    frozen = _sorted_unique(value["frozen_paths"], f"{PIN_PATH}.frozen_paths", allow_empty=True)
    for index, path in enumerate(frozen):
        _safe_path(path, f"{PIN_PATH}.frozen_paths[{index}]", directory_ok=True)
    return Pin(export_id, value["receipt_path"], digest, tuple(frozen))


def receipt_copy_problems(
    repo: pathlib.Path, commits: Iterable[str], fetch: Callable[[str], Any]
) -> list[str]:
    """Each pinned receipt copy whose blob differs from Vellum main's receipt.

    ``fetch`` takes a GitHub REST path and returns its decoded JSON; a fetch
    failure propagates, so an unreachable Vellum never reads as a match. The
    pin at each commit is checked, so the base pin that judges this change and
    a head re-pin that judges the next ones are both Vellum's bytes.
    """
    problems: list[str] = []
    seen: set[tuple[str, str]] = set()
    for commit in commits:
        raw = _blob(repo, commit, PIN_PATH, MAX_EVENT_BYTES)
        if raw is None:
            continue
        pin = parse_pin(raw)
        listing = _git(repo, "ls-tree", commit, "--", pin.receipt_path).stdout.decode()
        fields = listing.split()
        if len(fields) < 4 or fields[1] != "blob":
            problems.append(f"{commit[:12]}:{pin.receipt_path} is not a pinned file")
            continue
        local = fields[2]
        if (pin.export_id, local) in seen:
            continue
        seen.add((pin.export_id, local))
        source = f"provenance/mirror/receipts/{pin.export_id}.json"
        remote = fetch(f"repos/{FRAMEWORK_REPOSITORY}/contents/{source}?ref=main")
        if not isinstance(remote, dict) or remote.get("type") != "file" or remote.get("path") != source:
            problems.append(f"{FRAMEWORK_REPOSITORY} main has no file {source}")
        elif remote.get("sha") != local:
            problems.append(
                f"{pin.receipt_path} is blob {local[:12]}, but {FRAMEWORK_REPOSITORY} main's "
                f"{source} is {str(remote.get('sha'))[:12]}; the pinned copy is not Vellum's receipt"
            )
    return problems


def parse_receipt(raw: bytes, pin: Pin) -> Receipt:
    if hashlib.sha256(raw).hexdigest() != pin.receipt_sha256:
        raise ReceiptDriftError(
            f"{pin.receipt_path}: bytes differ from the pinned receipt_sha256; "
            "re-pin the receipt and its digest together"
        )
    value = _strict_json(raw, pin.receipt_path)
    if not isinstance(value, dict):
        raise ReceiptDriftError(f"{pin.receipt_path}: expected an object")
    if value.get("schema") != RECEIPT_SCHEMA:
        raise ReceiptDriftError(f"{pin.receipt_path}: schema must be {RECEIPT_SCHEMA}")
    if value.get("export_id") != pin.export_id:
        raise ReceiptDriftError(f"{pin.receipt_path}: export_id differs from the pin")
    if value.get("pulp_repository") != PULP_REPOSITORY:
        raise ReceiptDriftError(f"{pin.receipt_path}: pulp_repository must be {PULP_REPOSITORY}")
    pulp_sha = value.get("pulp_sha")
    if not isinstance(pulp_sha, str) or not SHA40_RE.fullmatch(pulp_sha):
        raise ReceiptDriftError(f"{pin.receipt_path}: pulp_sha must be a full commit SHA")
    if pulp_sha[:12] != pin.export_id.split("-", 1)[1]:
        raise ReceiptDriftError(f"{pin.receipt_path}: pulp_sha does not match the export_id")
    files = value.get("files")
    if not isinstance(files, list) or not files:
        raise ReceiptDriftError(f"{pin.receipt_path}: files must be a non-empty array")
    exported: set[str] = set()
    for index, row in enumerate(files):
        if not isinstance(row, dict):
            raise ReceiptDriftError(f"{pin.receipt_path}: files[{index}] must be an object")
        exported.add(_safe_path(row.get("pulp_path"), f"{pin.receipt_path}: files[{index}].pulp_path"))
    if len(exported) != len(files):
        raise ReceiptDriftError(f"{pin.receipt_path}: files repeat a pulp_path")
    producers: set[str] = set()
    generated = value.get("generated_inputs", [])
    if not isinstance(generated, list):
        raise ReceiptDriftError(f"{pin.receipt_path}: generated_inputs must be an array")
    for index, row in enumerate(generated):
        paths = row.get("producer_paths") if isinstance(row, dict) else None
        if not isinstance(paths, list):
            raise ReceiptDriftError(
                f"{pin.receipt_path}: generated_inputs[{index}].producer_paths must be an array"
            )
        for path_index, path in enumerate(paths):
            producers.add(_safe_path(
                path,
                f"{pin.receipt_path}: generated_inputs[{index}].producer_paths[{path_index}]",
            ))
    source_dirs = {
        posixpath.dirname(path)
        for path in exported
        if posixpath.splitext(path)[1] in SOURCE_SUFFIXES and "/" in path
    }
    return Receipt(pin.export_id, pulp_sha, frozenset(exported), frozenset(producers),
                   frozenset(source_dirs))


def _changes(repo: pathlib.Path, base: str, head: str) -> list[tuple[str, str]]:
    # --ignore-submodules=none: a gitlink added under an exported directory is
    # a change even when .gitmodules says ignore = all.
    raw = _git(repo, "diff", "--name-status", "-z", "--no-renames", "--no-ext-diff",
               "--ignore-submodules=none", base, head, "--").stdout.split(b"\0")
    if raw and raw[-1] == b"":
        raw.pop()
    if len(raw) % 2:
        raise ReceiptDriftError("git diff returned a truncated name-status record")
    rows = []
    for index in range(0, len(raw), 2):
        status = raw[index].decode("ascii")
        try:
            path = raw[index + 1].decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ReceiptDriftError("changed paths must be valid UTF-8") from exc
        rows.append((status[:1], path))
    return rows


def _is_frozen(path: str, frozen: Iterable[str]) -> bool:
    return any(path == item or (item.endswith("/") and path.startswith(item)) for item in frozen)


def _under(path: str, directories: frozenset[str]) -> bool:
    parent = posixpath.dirname(path)
    while parent:
        if parent in directories:
            return True
        parent = posixpath.dirname(parent)
    return False


def drifted_paths(
    changes: Iterable[tuple[str, str]],
    receipt: Receipt,
    pin: Pin,
    existed_at_export: Callable[[str], bool] = lambda path: True,
) -> dict[str, str]:
    """Changed paths that owe an inheritance decision, with the reason each one does.

    A file under an exported source directory that did not exist at the export
    commit is one the next re-export picks up, so every change to it owes a
    decision, not only the one that adds it. A file that existed then and was
    not exported (another platform's source, say) owes nothing.
    """
    owed: dict[str, str] = {}
    for status, path in changes:
        if _is_frozen(path, pin.frozen_paths):
            continue
        if path in receipt.exported:
            owed[path] = "exported"
        elif path in receipt.producers:
            owed[path] = "produces a generated export input"
        elif _under(path, receipt.source_dirs):
            if status == "A":
                owed[path] = "added in an exported source directory"
            elif not existed_at_export(path):
                owed[path] = "added to an exported source directory since the export"
    return owed


def frozen_edits(changes: Iterable[tuple[str, str]], pin: Pin) -> list[str]:
    """Changed paths the pin freezes: each needs a backport or emergency event."""
    return sorted({path for _, path in changes if _is_frozen(path, pin.frozen_paths)})


def frozen_edit_coverage(
    repo: pathlib.Path,
    head: str,
    changes: list[tuple[str, str]],
    edits: list[str],
    slice_paths: dict[str, list[str]],
) -> set[str]:
    """Frozen paths an added change event covers with an unfreezing disposition."""
    covered: set[str] = set()
    if not edits:
        return covered
    for status, path in changes:
        if (
            status != "A"
            or posixpath.dirname(path) != CHANGE_EVENT_DIRECTORY
            or not path.endswith(".json")
        ):
            continue
        raw = _blob(repo, head, path, MAX_EVENT_BYTES)
        if raw is None:
            continue
        event = _strict_json(raw, path)
        if (
            not isinstance(event, dict)
            or event.get("kind") != "change"
            or event.get("disposition") not in FROZEN_EDIT_DISPOSITIONS
        ):
            continue
        slices = event.get("slices", [])
        if not isinstance(slices, list) or not all(isinstance(x, str) for x in slices):
            raise ReceiptDriftError(f"{path}: slices must be a list of slice ids")
        patterns = [pattern for slice_id in slices for pattern in slice_paths.get(slice_id, [])]
        covered.update(item for item in edits if any(_slice_matches(item, p) for p in patterns))
    return covered


def _existed_at(repo: pathlib.Path, commit: str) -> Callable[[str], bool]:
    def existed(path: str) -> bool:
        return _git(repo, "cat-file", "-e", f"{commit}:{path}", check=False).returncode == 0

    return existed


def _require_commit(repo: pathlib.Path, sha: str, head: str, where: str) -> None:
    """The receipt's Pulp commit must be real Pulp history, reachable from head."""
    if _git(repo, "cat-file", "-e", f"{sha}^{{commit}}", check=False).returncode:
        raise ReceiptDriftError(
            f"{where}: pulp_sha {sha} is not in this checkout; the check needs full history"
        )
    if _git(repo, "merge-base", "--is-ancestor", sha, head, check=False).returncode:
        raise ReceiptDriftError(f"{where}: pulp_sha {sha} is not an ancestor of {head}")


def _parse_utc(value: Any, where: str) -> None:
    if not isinstance(value, str) or not value.endswith("Z"):
        raise ReceiptDriftError(f"{where}: expected an ISO-8601 UTC timestamp ending in Z")
    try:
        dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError as exc:
        raise ReceiptDriftError(f"{where}: expected an ISO-8601 UTC timestamp") from exc


@dataclasses.dataclass(frozen=True)
class ExportEvent:
    event_id: str
    paths: tuple[str, ...]
    inherits: bool
    supersedes: tuple[str, ...]


EXPORT_EVENT_KEYS = frozenset({
    "schema_version", "event_id", "kind", "created_at", "receipt_export_id",
    "paths", "vellum_inherits", "rationale",
})


def validate_export_event(value: Any, path: str, pin: Pin) -> ExportEvent:
    keys = set(EXPORT_EVENT_KEYS)
    if isinstance(value, dict) and "supersedes" in value:
        keys.add("supersedes")
    event = _exact_keys(value, keys, path)
    if type(event["schema_version"]) is not int or event["schema_version"] != 1:
        raise ReceiptDriftError(f"{path}: schema_version must be 1")
    event_id = event["event_id"]
    if not isinstance(event_id, str) or not EVENT_ID_RE.fullmatch(event_id):
        raise ReceiptDriftError(f"{path}: event_id is malformed")
    if posixpath.basename(path) != f"{event_id}.json":
        raise ReceiptDriftError(f"{path}: file name must be {event_id}.json")
    if event["kind"] != "exported-path-change":
        raise ReceiptDriftError(f"{path}: kind must be exported-path-change")
    _parse_utc(event["created_at"], f"{path}.created_at")
    if event["receipt_export_id"] != pin.export_id:
        raise ReceiptDriftError(
            f"{path}: receipt_export_id must name the pinned receipt {pin.export_id}"
        )
    paths = _sorted_unique(event["paths"], f"{path}.paths", allow_empty=False)
    for index, item in enumerate(paths):
        _safe_path(item, f"{path}.paths[{index}]")
    if type(event["vellum_inherits"]) is not bool:
        raise ReceiptDriftError(f"{path}: vellum_inherits must be true or false")
    if not isinstance(event["rationale"], str) or len(event["rationale"].strip()) < 24:
        raise ReceiptDriftError(f"{path}: rationale must explain the decision")
    supersedes: list[str] = []
    if "supersedes" in event:
        supersedes = _sorted_unique(event["supersedes"], f"{path}.supersedes", allow_empty=False)
        for item in supersedes:
            if not EVENT_ID_RE.fullmatch(item) or item == event_id:
                raise ReceiptDriftError(
                    f"{path}: supersedes must name other export events by event_id"
                )
    return ExportEvent(event_id, tuple(paths), event["vellum_inherits"], tuple(supersedes))


def _apply_supersession(
    repo: pathlib.Path,
    head: str,
    pin: Pin,
    decisions: dict[str, dict[str, bool]],
    correcting: list[tuple[str, ExportEvent]],
) -> None:
    """Drop each superseded export event's value for the paths its correction lists."""
    for path, event in correcting:
        for superseded_id in event.supersedes:
            superseded_path = f"{EXPORT_EVENT_DIRECTORY}/{superseded_id}.json"
            raw = _blob(repo, head, superseded_path, MAX_EVENT_BYTES)
            if raw is None:
                raise ReceiptDriftError(
                    f"{path}: supersedes {superseded_id}, which is not an export event"
                )
            superseded = validate_export_event(
                _strict_json(raw, superseded_path), superseded_path, pin
            )
            unclaimed = [item for item in event.paths if item not in superseded.paths]
            if unclaimed:
                raise ReceiptDriftError(
                    f"{path}: lists paths {superseded_id} does not decide: "
                    + ", ".join(unclaimed)
                )
            for item in event.paths:
                decisions.get(item, {}).pop(superseded_path, None)


def _slice_matches(path: str, pattern: str) -> bool:
    """The freeze check's slice-path semantics (vellum_freeze_check._matches)."""
    if pattern.endswith("/"):
        return path.startswith(pattern)
    if any(marker in pattern for marker in "*?["):
        return pathlib.PurePosixPath(path).match(pattern)
    return path == pattern


def _transferred_slice_paths(base_map: dict[str, Any] | None) -> dict[str, list[str]]:
    if not isinstance(base_map, dict):
        return {}
    result: dict[str, list[str]] = {}
    for item in base_map.get("slices", []):
        if (
            isinstance(item, dict)
            and item.get("state") == "framework-authoritative-transferred"
            and isinstance(item.get("id"), str)
            and isinstance(item.get("paths"), list)
        ):
            result[item["id"]] = [path for path in item["paths"] if isinstance(path, str)]
    return result


def collect_decisions(
    repo: pathlib.Path,
    head: str,
    changes: list[tuple[str, str]],
    owed: dict[str, str],
    pin: Pin,
    slice_paths: dict[str, list[str]],
) -> tuple[dict[str, set[bool]], list[str]]:
    """Each owed path's stated inheritance values, and the events read."""
    decisions: dict[str, dict[str, bool]] = {path: {} for path in owed}
    correcting: list[tuple[str, ExportEvent]] = []
    events: list[str] = []
    for status, path in changes:
        directory = posixpath.dirname(path)
        if directory == EXPORT_EVENT_DIRECTORY:
            if status != "A":
                raise ReceiptDriftError(f"{path}: export events are append-only")
            if not path.endswith(".json"):
                raise ReceiptDriftError(f"{path}: export events are .json files")
            raw = _blob(repo, head, path, MAX_EVENT_BYTES)
            if raw is None:
                raise ReceiptDriftError(f"{path}: cannot read the added export event")
            event = validate_export_event(_strict_json(raw, path), path, pin)
            # A correction may name a path this change leaves alone: the path's
            # decision was recorded earlier, and _apply_supersession proves it.
            stray = [] if event.supersedes else [
                item for item in event.paths if item not in owed
            ]
            if stray:
                raise ReceiptDriftError(
                    f"{path}: claims paths this change does not owe a decision for: "
                    + ", ".join(stray)
                )
            for item in event.paths:
                if item in decisions:
                    decisions[item][path] = event.inherits
            if event.supersedes:
                correcting.append((path, event))
            events.append(path)
            continue
        if directory not in (CHANGE_EVENT_DIRECTORY, WATCH_EVENT_DIRECTORY) or status != "A":
            continue
        if not path.endswith(".json"):
            continue
        raw = _blob(repo, head, path, MAX_EVENT_BYTES)
        if raw is None:
            continue
        event = _strict_json(raw, path)
        if not isinstance(event, dict) or "vellum_inherits" not in event:
            continue
        inherits = event["vellum_inherits"]
        if type(inherits) is not bool:
            raise ReceiptDriftError(f"{path}: vellum_inherits must be true or false")
        if directory == CHANGE_EVENT_DIRECTORY:
            if event.get("kind") != "change":
                continue
            slices = event.get("slices", [])
            if not isinstance(slices, list) or not all(isinstance(x, str) for x in slices):
                raise ReceiptDriftError(f"{path}: slices must be a list of slice ids")
            patterns = [
                pattern
                for slice_id in slices
                for pattern in slice_paths.get(slice_id, [])
            ]
            covered = [
                item for item in owed if any(_slice_matches(item, p) for p in patterns)
            ]
        else:
            families = event.get("capability_families", [])
            if not isinstance(families, list) or not all(isinstance(x, str) for x in families):
                raise ReceiptDriftError(f"{path}: capability_families must be a list of names")
            patterns = [
                selector
                for family in families
                for selector in watch.EXPECTED_SCOPES.get(family, [])
            ]
            covered = [
                item for item in owed if any(watch._matches(item, p) for p in patterns)
            ]
        for item in covered:
            decisions[item][path] = inherits
        events.append(path)
    _apply_supersession(repo, head, pin, decisions, correcting)
    return {path: set(values.values()) for path, values in decisions.items()}, events


def check(repo: pathlib.Path, base: str, head: str) -> dict[str, Any]:
    """Judge ``base..head``. Raises ReceiptDriftError when the check cannot run."""
    base_pin_raw = _blob(repo, base, PIN_PATH, MAX_EVENT_BYTES)
    head_pin_raw = _blob(repo, head, PIN_PATH, MAX_EVENT_BYTES)
    if base_pin_raw is None and head_pin_raw is None:
        return {"status": "pass", "state": "inactive", "export_id": None,
                "owed": {}, "missing": [], "conflicting": [], "frozen": [], "events": []}
    if base_pin_raw is not None and head_pin_raw is None:
        raise ReceiptDriftError(
            f"{PIN_PATH} cannot be removed; retiring the receipt guard is a change "
            "to vellum_receipt_drift.py and its workflow, not to its data"
        )
    # The base pin judges the change. Only the change that introduces the pin
    # is judged by its own copy, because no earlier one exists.
    pin_commit = base if base_pin_raw is not None else head
    pin = parse_pin(base_pin_raw if base_pin_raw is not None else head_pin_raw)
    receipt_raw = _blob(repo, pin_commit, pin.receipt_path, MAX_RECEIPT_BYTES)
    if receipt_raw is None:
        raise ReceiptDriftError(f"{pin_commit[:12]}:{pin.receipt_path} is missing")
    receipt = parse_receipt(receipt_raw, pin)
    if head_pin_raw is not None and pin_commit == base:
        head_pin = parse_pin(head_pin_raw)
        head_receipt = _blob(repo, head, head_pin.receipt_path, MAX_RECEIPT_BYTES)
        if head_receipt is None:
            raise ReceiptDriftError(f"{head_pin.receipt_path} is missing at the head commit")
        new_receipt = parse_receipt(head_receipt, head_pin)
        _require_commit(repo, new_receipt.pulp_sha, head, head_pin.receipt_path)

    _require_commit(repo, receipt.pulp_sha, head, pin.receipt_path)
    changes = _changes(repo, base, head)
    owed = drifted_paths(changes, receipt, pin, _existed_at(repo, receipt.pulp_sha))
    map_raw = _blob(repo, base, MAP_PATH, MAX_RECEIPT_BYTES)
    base_map = _strict_json(map_raw, MAP_PATH) if map_raw is not None else None
    slice_paths = _transferred_slice_paths(base_map)
    decisions, events = collect_decisions(repo, head, changes, owed, pin, slice_paths)
    missing = sorted(path for path, values in decisions.items() if not values)
    conflicting = sorted(path for path, values in decisions.items() if len(values) > 1)
    edits = frozen_edits(changes, pin)
    unfrozen = frozen_edit_coverage(repo, head, changes, edits, slice_paths)
    frozen = [path for path in edits if path not in unfrozen]
    return {
        "status": "fail" if missing or conflicting or frozen else "pass",
        "state": "active",
        "export_id": pin.export_id,
        "owed": dict(sorted(owed.items())),
        "missing": missing,
        "conflicting": conflicting,
        "frozen": frozen,
        "events": sorted(events),
    }


def describe_failure(report: dict[str, Any]) -> str:
    if report.get("frozen") and not report["missing"] and not report["conflicting"]:
        return describe_frozen(report)
    lines = [
        f"changed paths exported by Vellum receipt {report['export_id']} need an "
        "explicit vellum_inherits decision"
    ]
    if report["missing"]:
        lines.append("")
        lines.append("  No added event states whether Vellum inherits these:")
        lines.extend(
            f"    - {path} ({report['owed'][path]})" for path in report["missing"]
        )
    if report["conflicting"]:
        lines.append("")
        lines.append("  Added events disagree about whether Vellum inherits these:")
        lines.extend(f"    - {path}" for path in report["conflicting"])
    lines.append("")
    lines.append(
        '  Fix: set "vellum_inherits": true or false on the change event or watch '
        "event that covers the path, or add"
    )
    lines.append(
        f"  {EXPORT_EVENT_DIRECTORY}/<event_id>.json (kind exported-path-change) "
        "listing the exact paths."
    )
    lines.append(
        "  See .agents/skills/pulp-vellum-change-routing/SKILL.md, "
        '"Receipt mode".'
    )
    if report.get("frozen"):
        lines.append("")
        lines.append(describe_frozen(report))
    return "\n".join(lines)


def describe_frozen(report: dict[str, Any]) -> str:
    lines = [
        f"changed paths are frozen in Pulp since Vellum export {report['export_id']}; "
        "their authoritative copy is Vellum's",
        "",
    ]
    lines.extend(f"    - {path}" for path in report["frozen"])
    lines.append("")
    lines.append(
        "  Fix: make the change in Vellum. To bring a Vellum commit back, or for an "
        "emergency, add a change event"
    )
    lines.append(
        f"  in {CHANGE_EVENT_DIRECTORY}/ naming the path's transferred slice with "
        "disposition framework-backport or emergency-exception."
    )
    return "\n".join(lines)
