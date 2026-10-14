#!/usr/bin/env python3
"""Check the sharded Vellum inheritance ledger.

Every change event that lands generic work in Pulp under the temporary
inheritance exception owes a ledger entry: one JSON file per event under
docs/contracts/vellum-inheritance-ledger/, or a row in the frozen ledger that
predates the directory. This covers both the original change-event records
and the expansion-watch records used by the extraction review. The checker
keeps the frozen file frozen, validates every entry against its event, and,
with --base, rejects edits to or deletion of an existing entry and requires
every event added since the base whose rationale mentions Vellum and
inheritance to state `vellum_inherits` explicitly. `--base auto` resolves the
base from the GitHub event in Actions (a merge-group checkout has no
origin/main) and to origin/main elsewhere.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
FROZEN_LEDGER = Path("docs/contracts/vellum-inheritance-ledger.md")
FROZEN_LEDGER_SHA256 = (
    "cf603315ccef0c10be49b14a071387e7df9815a289941c4a9f104b1cca741361"
)
ENTRIES_DIR = Path("docs/contracts/vellum-inheritance-ledger")
EVENTS_DIR = Path(".github/vellum-change-events")
EXPANSION_EVENTS_DIR = Path(".github/vellum-expansion-watch-events")
SCHEMA = "pulp.vellum-inheritance-entry.v1"
ENTRY_KEYS = {"schema", "event_id", "slices", "vellum_obligation", "evidence"}
INHERITANCE_PREFIX = (
    "design-import-refactor: generic change landed in Pulp per owner decision "
    "2026-10-04; Vellum inherits (see inheritance ledger)"
)
# An affirmative inheritance claim in free text: "Vellum inherits",
# "Vellum's JS UI runtime inherits", "inherited by Vellum". Advisory phrasing
# ("Vellum should inherit", "not a shape Vellum should inherit") is not a
# claim, and "inherits nothing" is an explicit non-claim.
INHERITANCE_CLAIM = re.compile(
    r"\bVellum(?:'s(?:\s+[\w-]+){1,6})?\s+inherits\b(?!\s+nothing)"
    r"|(?<!not\s)\binherited\s+by\s+Vellum\b",
    re.IGNORECASE,
)
MENTIONS_VELLUM_INHERITANCE = re.compile(
    r"vellum.*inherit|inherit.*vellum", re.IGNORECASE | re.DOTALL
)
FULL_SHA = re.compile(r"[0-9a-fA-F]{40}")
EVENT_ID = re.compile(r"20[0-9]{6}-[a-z0-9]+(?:-[a-z0-9]+)*\Z")


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"{path}: cannot read JSON: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def claims_vellum_inheritance(event: dict[str, Any]) -> bool:
    """Whether an event records that Vellum inherits its change.

    An explicit boolean ``vellum_inherits`` field decides on its own. Without
    it, the legacy rationale prefix or an affirmative claim in the rationale
    counts.
    """
    explicit = event.get("vellum_inherits")
    if isinstance(explicit, bool):
        return explicit
    rationale = event.get("rationale")
    if not isinstance(rationale, str):
        return False
    return rationale.startswith(INHERITANCE_PREFIX) or bool(
        INHERITANCE_CLAIM.search(rationale)
    )


def frozen_event_ids(text: str) -> set[str]:
    return set(re.findall(r"^\| `(20[0-9]{6}-[a-z0-9-]+)`", text, re.MULTILINE))


def check_entry(root: Path, path: Path) -> list[str]:
    errors: list[str] = []
    try:
        entry = load_object(path)
    except ValueError as error:
        return [str(error)]
    where = path.relative_to(root).as_posix()
    if set(entry) != ENTRY_KEYS:
        return [
            f"{where}: keys differ; missing={sorted(ENTRY_KEYS - set(entry))} "
            f"unexpected={sorted(set(entry) - ENTRY_KEYS)}"
        ]
    if entry["schema"] != SCHEMA:
        errors.append(f"{where}: schema must be {SCHEMA!r}")
    event_id = entry["event_id"]
    if not isinstance(event_id, str) or EVENT_ID.fullmatch(event_id) is None:
        return errors + [f"{where}: event_id is not a change-event id"]
    if path.name != f"{event_id}.json":
        errors.append(f"{where}: file name must be {event_id}.json")
    if not isinstance(entry["vellum_obligation"], str) or not entry["vellum_obligation"].strip():
        errors.append(f"{where}: vellum_obligation must be non-empty text")
    evidence = entry["evidence"]
    if not isinstance(evidence, list) or not evidence or not all(
        isinstance(item, str) and item.strip() for item in evidence
    ):
        errors.append(f"{where}: evidence must be a non-empty list of text")
    event_paths = [root / EVENTS_DIR / f"{event_id}.json",
                   root / EXPANSION_EVENTS_DIR / f"{event_id}.json"]
    event_path = next((candidate for candidate in event_paths if candidate.is_file()), None)
    if event_path is None:
        return errors + [
            f"{where}: no event in {EVENTS_DIR} or {EXPANSION_EVENTS_DIR} for {event_id}"
        ]
    try:
        event = load_object(event_path)
    except ValueError as error:
        return errors + [str(error)]
    event_slices = event.get("slices", event.get("capability_families"))
    if entry["slices"] != event_slices:
        errors.append(
            f"{where}: slices {entry['slices']!r} differ from the change event's "
            f"{event_slices!r}"
        )
    return errors


class BaseError(RuntimeError):
    """The comparison base cannot be resolved; the caller reports it."""


def _git(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(root), *args],
        text=True, encoding="utf-8", stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )


def event_base_sha() -> str | None:
    """The base commit a GitHub Actions run names in its event payload.

    merge_group runs name the queue entry's own base (merge_group.base_sha);
    pull_request runs name the base branch tip their merge ref was built on
    (pull_request.base.sha). Neither checkout carries origin/main, and both
    check out a commit that contains that base, so the base-to-HEAD tree diff
    is exactly this change. Outside Actions there is no event.
    """
    if os.environ.get("GITHUB_ACTIONS") != "true":
        return None
    event_path = os.environ.get("GITHUB_EVENT_PATH")
    if not event_path:
        return None
    try:
        event = json.loads(Path(event_path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BaseError(f"cannot read the GitHub event {event_path}: {error}") from error
    if not isinstance(event, dict):
        raise BaseError(f"GitHub event {event_path} is not an object")
    candidates = []
    merge_group = event.get("merge_group")
    if isinstance(merge_group, dict):
        candidates.append(merge_group.get("base_sha"))
    pull_request = event.get("pull_request")
    if isinstance(pull_request, dict) and isinstance(pull_request.get("base"), dict):
        candidates.append(pull_request["base"].get("sha"))
    for candidate in candidates:
        if isinstance(candidate, str) and FULL_SHA.fullmatch(candidate):
            return candidate.lower()
    return None


def resolve_base(root: Path, base: str) -> tuple[str, str]:
    """Resolve --base to (revision, diff range form).

    ``auto`` takes the event's base commit in GitHub Actions, fetching it at
    depth 1 when the checkout lacks it, and compares trees directly (two-dot):
    the checked-out commit already contains that base. Elsewhere ``auto`` means
    origin/main, compared from the merge-base (three-dot) so a branch behind
    main is not charged with main's newer entries. An explicit ref keeps the
    three-dot form.
    """
    if base != "auto":
        return base, "..."
    sha = event_base_sha()
    if sha is None:
        return "origin/main", "..."
    if _git(root, "cat-file", "-e", f"{sha}^{{commit}}").returncode != 0:
        # No detached auto-maintenance: it would outlive this process.
        fetched = _git(root, "-c", "maintenance.auto=false", "-c", "gc.auto=0",
                       "fetch", "--no-tags", "--depth=1", "origin", sha)
        if fetched.returncode != 0:
            raise BaseError(
                f"event base {sha} is not in the checkout and could not be fetched: "
                + (fetched.stderr.strip() or "git fetch failed")
            )
    return sha, ".."


def changed_paths(root: Path, base: str, form: str, paths: list[str]) -> list[tuple[str, str]]:
    completed = _git(root, "diff", "--name-status", "--no-renames",
                     f"{base}{form}HEAD", "--", *paths)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or "git could not resolve the requested ref"
        raise BaseError(f"--base ref {base!r} is unavailable: {detail}")
    rows = []
    for line in completed.stdout.splitlines():
        status, _, path = line.partition("\t")
        rows.append((status, path))
    return rows


def changed_entries(root: Path, base: str, form: str = "...") -> list[str]:
    try:
        rows = changed_paths(root, base, form, [ENTRIES_DIR.as_posix()])
    except BaseError as error:
        return [str(error)]
    errors = []
    for status, path in rows:
        if status != "A" and path.endswith(".json"):
            errors.append(
                f"{path}: ledger entries are append-only (status {status}); "
                "add a new entry instead"
            )
    return errors


def unresolved_new_claims(root: Path, base: str, form: str = "...") -> list[str]:
    """New events whose rationale mentions Vellum and inheritance but that do
    not say with ``vellum_inherits`` whether Vellum inherits them. Free-text
    detection is a backstop for the events already on main; a new event states
    its intent explicitly."""
    try:
        rows = changed_paths(
            root, base, form, [EVENTS_DIR.as_posix(), EXPANSION_EVENTS_DIR.as_posix()]
        )
    except BaseError as error:
        return [str(error)]
    errors = []
    for status, path in rows:
        if status != "A" or not path.endswith(".json"):
            continue
        try:
            event = load_object(root / path)
        except ValueError as error:
            errors.append(str(error))
            continue
        rationale = event.get("rationale")
        if (
            "vellum_inherits" not in event
            and isinstance(rationale, str)
            and MENTIONS_VELLUM_INHERITANCE.search(rationale)
        ):
            errors.append(
                f"{path}: the rationale mentions Vellum and inheritance; set "
                '"vellum_inherits": true or false to state whether Vellum inherits '
                "this change"
            )
    return errors


def check(root: Path, base: str | None = None) -> list[str]:
    errors: list[str] = []
    frozen_path = root / FROZEN_LEDGER
    try:
        frozen_bytes = frozen_path.read_bytes()
    except OSError as error:
        return [f"cannot read {FROZEN_LEDGER}: {error}"]
    if hashlib.sha256(frozen_bytes).hexdigest() != FROZEN_LEDGER_SHA256:
        errors.append(
            f"{FROZEN_LEDGER} is frozen; record new entries as files under "
            f"{ENTRIES_DIR}/ instead"
        )
    recorded = frozen_event_ids(frozen_bytes.decode("utf-8"))
    entries_dir = root / ENTRIES_DIR
    if entries_dir.is_dir():
        for path in sorted(entries_dir.iterdir()):
            if path.name == "README.md":
                continue
            if path.suffix != ".json" or not path.is_file():
                errors.append(f"{path.relative_to(root).as_posix()}: unexpected file")
                continue
            entry_errors = check_entry(root, path)
            errors.extend(entry_errors)
            if not entry_errors:
                event_id = path.stem
                if event_id in recorded:
                    errors.append(
                        f"{path.relative_to(root).as_posix()}: event already has a "
                        f"row in {FROZEN_LEDGER}"
                    )
                recorded.add(event_id)
    event_paths = list((root / EVENTS_DIR).glob("*.json"))
    event_paths.extend((root / EXPANSION_EVENTS_DIR).glob("*.json"))
    for event_path in sorted(event_paths):
        try:
            event = load_object(event_path)
        except ValueError as error:
            errors.append(str(error))
            continue
        claims_inheritance = claims_vellum_inheritance(event)
        if claims_inheritance:
            event_id = event.get("event_id", event_path.stem)
            if event_id not in recorded:
                errors.append(
                    f"{event_path.relative_to(root).as_posix()}: event {event_id} "
                    f"claims Vellum inherits it but has no entry in {ENTRIES_DIR}/ "
                    f"or {FROZEN_LEDGER}"
                )
    if base is not None:
        try:
            revision, form = resolve_base(root, base)
        except BaseError as error:
            return errors + [str(error)]
        errors.extend(changed_entries(root, revision, form))
        errors.extend(unresolved_new_claims(root, revision, form))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument(
        "--base",
        help="reject edits to entries since this ref; 'auto' uses the GitHub "
             "event's base commit in Actions and origin/main elsewhere",
    )
    args = parser.parse_args(argv)
    errors = check(args.root.resolve(), args.base)
    for error in errors:
        print(f"vellum-inheritance-ledger: {error}", file=sys.stderr)
    if errors:
        return 1
    print("vellum-inheritance-ledger: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
