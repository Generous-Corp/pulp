#!/usr/bin/env python3
"""Check the sharded Vellum inheritance ledger.

Every change event that lands generic work in Pulp under the temporary
inheritance exception owes a ledger entry: one JSON file per event under
docs/contracts/vellum-inheritance-ledger/, or a row in the frozen ledger that
predates the directory. This checker keeps the frozen file frozen, validates
every entry against its change event, and, with --base, rejects edits to or
deletion of an existing entry.
"""

from __future__ import annotations

import argparse
import hashlib
import json
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
SCHEMA = "pulp.vellum-inheritance-entry.v1"
ENTRY_KEYS = {"schema", "event_id", "slices", "vellum_obligation", "evidence"}
INHERITANCE_PREFIX = (
    "design-import-refactor: generic change landed in Pulp per owner decision "
    "2026-10-04; Vellum inherits (see inheritance ledger)"
)
EVENT_ID = re.compile(r"20[0-9]{6}-[a-z0-9]+(?:-[a-z0-9]+)*\Z")


def load_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"{path}: cannot read JSON: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


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
    event_path = root / EVENTS_DIR / f"{event_id}.json"
    if not event_path.is_file():
        return errors + [f"{where}: no change event {EVENTS_DIR / (event_id + '.json')}"]
    try:
        event = load_object(event_path)
    except ValueError as error:
        return errors + [str(error)]
    if entry["slices"] != event.get("slices"):
        errors.append(
            f"{where}: slices {entry['slices']!r} differ from the change event's "
            f"{event.get('slices')!r}"
        )
    return errors


def changed_entries(root: Path, base: str) -> list[str]:
    completed = subprocess.run(
        ["git", "-C", str(root), "diff", "--name-status", "--no-renames",
         f"{base}...HEAD", "--", ENTRIES_DIR.as_posix()],
        check=True, text=True, stdout=subprocess.PIPE,
    )
    errors = []
    for line in completed.stdout.splitlines():
        status, _, path = line.partition("\t")
        if status != "A" and path.endswith(".json"):
            errors.append(
                f"{path}: ledger entries are append-only (status {status}); "
                "add a new entry instead"
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
    for event_path in sorted((root / EVENTS_DIR).glob("*.json")):
        try:
            event = load_object(event_path)
        except ValueError as error:
            errors.append(str(error))
            continue
        rationale = event.get("rationale")
        if isinstance(rationale, str) and rationale.startswith(INHERITANCE_PREFIX):
            event_id = event.get("event_id", event_path.stem)
            if event_id not in recorded:
                errors.append(
                    f"change event {event_id} carries the inheritance prefix but has "
                    f"no entry in {ENTRIES_DIR}/ or {FROZEN_LEDGER}"
                )
    if base is not None:
        errors.extend(changed_entries(root, base))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--base", help="reject edits to entries since this ref")
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
