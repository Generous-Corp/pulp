#!/usr/bin/env python3
"""Negative controls for the sharded Vellum inheritance ledger checker."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("vellum_inheritance_ledger_check.py")
SPEC = importlib.util.spec_from_file_location("vellum_inheritance_ledger_check", SCRIPT)
assert SPEC and SPEC.loader
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)
ROOT = SCRIPT.parents[2]
EVENT_ID = "20990101-example-generic-change"


class Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        shutil.copytree(ROOT / checker.EVENTS_DIR, self.root / checker.EVENTS_DIR)
        (self.root / checker.ENTRIES_DIR).mkdir(parents=True)
        shutil.copy2(ROOT / checker.FROZEN_LEDGER, self.root / checker.FROZEN_LEDGER)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_event(self, *, prefixed: bool = True, slices=("retained-ui-kernel",)) -> None:
        rationale = (checker.INHERITANCE_PREFIX + "; example.") if prefixed else "Audio-only."
        (self.root / checker.EVENTS_DIR / f"{EVENT_ID}.json").write_text(
            json.dumps({
                "schema_version": 1, "event_id": EVENT_ID, "kind": "change",
                "created_at": "2099-01-01T00:00:00Z", "slices": list(slices),
                "rationale": rationale, "tests": ["x"], "disposition": "pulp-only",
            }),
            encoding="utf-8",
        )

    def write_entry(self, **overrides) -> Path:
        entry = {
            "schema": checker.SCHEMA, "event_id": EVENT_ID,
            "slices": ["retained-ui-kernel"],
            "vellum_obligation": "Absorb the example change.",
            "evidence": ["pulp-test-example"],
        }
        entry.update(overrides)
        path = self.root / checker.ENTRIES_DIR / f"{EVENT_ID}.json"
        path.write_text(json.dumps(entry), encoding="utf-8")
        return path

    def test_repository_passes(self) -> None:
        self.assertEqual(checker.check(ROOT), [])

    def test_copied_tree_passes_before_mutation(self) -> None:
        self.assertEqual(checker.check(self.root), [])

    def test_prefixed_event_without_entry_is_rejected(self) -> None:
        self.write_event()
        errors = checker.check(self.root)
        self.assertTrue(any("has no entry" in error for error in errors), errors)

    def test_prefixed_event_with_entry_passes(self) -> None:
        self.write_event()
        self.write_entry()
        self.assertEqual(checker.check(self.root), [])

    def test_unprefixed_event_owes_no_entry(self) -> None:
        self.write_event(prefixed=False)
        self.assertEqual(checker.check(self.root), [])

    def test_entry_slices_must_match_event(self) -> None:
        self.write_event()
        self.write_entry(slices=["canvas-kernel"])
        errors = checker.check(self.root)
        self.assertTrue(any("slices" in error for error in errors), errors)

    def test_entry_for_missing_event_is_rejected(self) -> None:
        self.write_entry()
        errors = checker.check(self.root)
        self.assertTrue(any("no change event" in error for error in errors), errors)

    def test_entry_schema_and_keys_are_exact(self) -> None:
        self.write_event()
        self.write_entry(extra="x")
        self.assertTrue(any("keys differ" in e for e in checker.check(self.root)))
        self.write_entry(schema="other")
        self.assertTrue(any("schema" in e for e in checker.check(self.root)))
        self.write_entry(evidence=[])
        self.assertTrue(any("evidence" in e for e in checker.check(self.root)))

    def test_entry_file_name_must_match_event_id(self) -> None:
        self.write_event()
        path = self.write_entry()
        path.rename(path.with_name("misnamed-20990101.json"))
        errors = checker.check(self.root)
        self.assertTrue(errors, "a misnamed entry must not count")

    def test_frozen_ledger_cannot_grow(self) -> None:
        frozen = self.root / checker.FROZEN_LEDGER
        frozen.write_text(
            frozen.read_text(encoding="utf-8") + "| `20990101-x` | - | - | - | - |\n",
            encoding="utf-8",
        )
        errors = checker.check(self.root)
        self.assertTrue(any("is frozen" in error for error in errors), errors)

    def test_existing_entry_cannot_be_edited(self) -> None:
        def git(*args: str) -> None:
            subprocess.run(["git", "-C", str(self.root), *args], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        git("init", "-q")
        git("config", "user.email", "t@example.com")
        git("config", "user.name", "t")
        self.write_event()
        path = self.write_entry()
        git("add", "-A")
        git("commit", "-qm", "base")
        self.assertEqual(checker.check(self.root, "HEAD"), [])
        path.write_text(path.read_text(encoding="utf-8").replace("Absorb", "Ignore"),
                        encoding="utf-8")
        git("commit", "-qam", "edit")
        errors = checker.check(self.root, "HEAD~1")
        self.assertTrue(any("append-only" in error for error in errors), errors)


if __name__ == "__main__":
    unittest.main()
