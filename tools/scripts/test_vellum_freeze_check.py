#!/usr/bin/env python3
"""Unit and real-Git negative controls for vellum_freeze_check.py."""

from __future__ import annotations

import datetime as dt
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("vellum_freeze_check.py")
SPEC = importlib.util.spec_from_file_location("vellum_freeze_check", MODULE_PATH)
assert SPEC and SPEC.loader
freeze = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = freeze
SPEC.loader.exec_module(freeze)


def mapping(state: str = "active", slice_state: str = "framework-authoritative-transferred"):
    return {
        "schema_version": 1,
        "framework_repository": "Generous-Corp/vellum",
        "freeze_owner": "@danielraffel",
        "activation": {
            "state": state,
            "pulp_extraction_base": "a" * 40,
            "vellum_authority_commit": "b" * 40 if state == "active" else None,
            "accepted_by": "@danielraffel" if state == "active" else None,
            "accepted_at": "2026-07-22T20:00:00Z" if state == "active" else None,
        },
        "slices": [
            {
                "id": "design-ir",
                "state": slice_state,
                "paths": ["core/view/src/design_ir_json.cpp"],
            }
        ],
    }


def change_event(event_id: str = "20260722-design-fix"):
    return {
        "schema_version": 1,
        "event_id": event_id,
        "kind": "change",
        "created_at": "2026-07-22T20:00:00Z",
        "slices": ["design-ir"],
        "rationale": "Pulp-specific adapter behavior only.",
        "tests": ["design-contract"],
        "disposition": "pulp-only",
    }


class FreezeUnitTests(unittest.TestCase):
    def test_prepared_map_transfers_nothing(self):
        value = mapping("prepared", "pulp-authoritative-untransferred")
        freeze.validate_map(value)
        self.assertEqual(
            freeze.affected_slices([value], ["core/view/src/design_ir_json.cpp"]), {}
        )

    def test_active_map_matches_globs_and_exact_paths(self):
        value = mapping()
        freeze.validate_map(value)
        self.assertEqual(
            freeze.affected_slices(
                [value],
                [
                    "README.md",
                    "core/view/src/design_ir_json.cpp",
                ],
            ),
            {
                "design-ir": ["core/view/src/design_ir_json.cpp"]
            },
        )

    def test_active_authority_cannot_be_rewritten(self):
        base = mapping()
        head = json.loads(json.dumps(base))
        head["activation"]["vellum_authority_commit"] = "c" * 40
        with self.assertRaisesRegex(freeze.FreezeError, "immutable"):
            freeze.validate_map_transition(base, head)

    def test_transferred_slice_cannot_be_removed(self):
        base = mapping()
        head = json.loads(json.dumps(base))
        head["slices"] = []
        with self.assertRaisesRegex(freeze.FreezeError, "cannot change"):
            freeze.validate_map_transition(base, head)

    def test_activation_reports_exact_new_slice(self):
        base = mapping("prepared", "pulp-authoritative-untransferred")
        head = mapping()
        self.assertEqual(
            freeze.validate_map_transition(base, head), {"design-ir"}
        )

    def test_emergency_is_short_lived_and_issue_backed(self):
        event = change_event("20260722-emergency")
        event.update(
            {
                "disposition": "emergency-exception",
                "owner": "@owner",
                "expiry": "2026-08-20",
                "follow_up": "https://github.com/Generous-Corp/vellum/issues/1",
            }
        )
        with self.assertRaisesRegex(freeze.FreezeError, "within 14 days"):
            freeze.validate_event(
                event, ".github/vellum-change-events/20260722-emergency.json"
            )

    def test_unknown_event_fields_fail_closed(self):
        event = change_event()
        event["surprise"] = True
        with self.assertRaisesRegex(freeze.FreezeError, "unknown fields"):
            freeze.validate_event(
                event, ".github/vellum-change-events/20260722-design-fix.json"
            )

    def test_nested_event_path_cannot_duplicate_a_durable_event_id(self):
        with self.assertRaisesRegex(freeze.FreezeError, "direct .json child"):
            freeze.validate_event(
                change_event(),
                ".github/vellum-change-events/nested/20260722-design-fix.json",
            )

    def test_unresolved_manifest_row_cannot_transfer(self):
        manifest = {
            "source_commit": "a" * 40,
            "entries": [
                {
                    "source_path": "core/view/src/design_ir_json.cpp",
                    "classification": "unresolved",
                }
            ],
        }
        with self.assertRaisesRegex(freeze.FreezeError, "forbidden cut classification"):
            freeze.validate_transferred_projection(mapping(), manifest)

    def test_test_only_manifest_row_can_transfer(self):
        manifest = {
            "source_commit": "a" * 40,
            "entries": [
                {
                    "source_path": "core/view/src/design_ir_json.cpp",
                    "classification": "test-only",
                }
            ],
        }
        freeze.validate_transferred_projection(mapping(), manifest)

    def test_same_slice_cannot_receive_conflicting_events(self):
        first = change_event("20260722-pulp-only")
        second = change_event("20260722-backport")
        second.update({"disposition": "framework-backport", "framework_commit": "c" * 40})
        with self.assertRaisesRegex(freeze.FreezeError, "exactly match"):
            freeze._validate_event_coverage(
                [("first.json", first), ("second.json", second)],
                {"design-ir": ["core/view/src/design_ir_json.cpp"]},
                set(),
                None,
                None,
            )

    def test_historical_emergency_document_remains_parseable_after_expiry(self):
        event = change_event("20260722-replay")
        event.update({
            "disposition": "emergency-exception",
            "owner": "@owner",
            "expiry": "2026-07-29",
            "follow_up": "https://github.com/Generous-Corp/vellum/issues/1",
        })
        freeze.validate_event(
            event,
            ".github/vellum-change-events/20260722-replay.json",
        )

    def test_new_emergency_must_be_live_at_its_source_commit(self):
        event = change_event("20260722-stale-emergency")
        event.update({
            "disposition": "emergency-exception",
            "owner": "@owner",
            "expiry": "2026-07-29",
            "follow_up": "https://github.com/Generous-Corp/vellum/issues/1",
        })
        with self.assertRaisesRegex(freeze.FreezeError, "already expired"):
            freeze.validate_event(
                event,
                ".github/vellum-change-events/20260722-stale-emergency.json",
                commit_time=dt.datetime(2026, 7, 30, tzinfo=dt.timezone.utc),
            )

    def test_nonexistent_authority_commit_is_rejected(self):
        event = {
            "kind": "authority-transition",
            "vellum_authority_commit": "b" * 40,
            "counterpart": "provenance/activation/pending.json",
        }
        with mock.patch.object(
            freeze, "_github_json", return_value={"sha": "c" * 40}
        ):
            with self.assertRaisesRegex(freeze.FreezeError, "resolve exactly"):
                freeze.verify_authority_counterpart(
                    events=[("authority.json", event)],
                    transferred={"design-ir"},
                    head_map=mapping(),
                    token="redacted-test-token",
                )

    def test_nonexistent_backport_commit_is_rejected(self):
        event = change_event("20260722-backport-proof")
        event.update({"disposition": "framework-backport", "framework_commit": "d" * 40})
        with mock.patch.object(
            freeze, "_github_json", return_value={"sha": "e" * 40}
        ):
            with self.assertRaisesRegex(freeze.FreezeError, "did not resolve"):
                freeze.verify_framework_backports(
                    [("backport.json", event)], "redacted-test-token"
                )


class FreezeGitIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.repo = pathlib.Path(self.temporary.name)
        self._git("init", "-q")
        self._git("config", "user.name", "Freeze Test")
        self._git("config", "user.email", "freeze@example.invalid")
        self._write(".github/vellum-ownership.json", mapping())
        self._write(
            "docs/contracts/vellum-initial-cut-manifest.json",
            {
                "source_commit": "a" * 40,
                "entries": [
                    {
                        "source_path": "core/view/src/design_ir_json.cpp",
                        "classification": "framework-core",
                    }
                ],
            },
        )
        self._write("core/view/src/design_ir_json.cpp", "before\n")
        self._git("add", ".")
        self._git("commit", "-qm", "base")
        self.base = self._git("rev-parse", "HEAD").strip()

    def tearDown(self):
        self.temporary.cleanup()

    def _git(self, *args: str) -> str:
        return subprocess.check_output(
            ["git", "-C", str(self.repo), *args], text=True
        )

    def _write(self, relative: str, value):
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(value, dict):
            value = json.dumps(value, indent=2, sort_keys=True) + "\n"
        path.write_text(value, encoding="utf-8")

    def _commit(self, message: str) -> str:
        self._git("add", "-A")
        self._git("commit", "-qm", message)
        return self._git("rev-parse", "HEAD").strip()

    def _run(self, head: str) -> int:
        return freeze.main(
            [
                "--repo", str(self.repo),
                "--base", self.base,
                "--head", head,
                "--output", str(self.repo / "outbox.json"),
            ]
        )

    def test_frozen_change_without_event_fails(self):
        self._write("core/view/src/design_ir_json.cpp", "after\n")
        head = self._commit("missing event")
        self.assertEqual(self._run(head), 1)

    def test_frozen_change_with_added_event_passes(self):
        self._write("core/view/src/design_ir_json.cpp", "after\n")
        self._write(
            ".github/vellum-change-events/20260722-design-fix.json",
            change_event(),
        )
        head = self._commit("classified change")
        self.assertEqual(self._run(head), 0)
        outbox = json.loads((self.repo / "outbox.json").read_text())
        self.assertEqual(
            outbox["event_refs"][0]["path"],
            ".github/vellum-change-events/20260722-design-fix.json",
        )
        self.assertRegex(outbox["event_refs"][0]["sha256"], r"^[0-9a-f]{64}$")

    def test_existing_event_is_append_only(self):
        path = ".github/vellum-change-events/20260722-existing.json"
        self._write(path, change_event("20260722-existing"))
        self.base = self._commit("event baseline")
        event = change_event("20260722-existing")
        event["rationale"] = "Rewritten after review"
        self._write(path, event)
        head = self._commit("rewrite event")
        self.assertEqual(self._run(head), 1)

    def test_rename_out_of_frozen_path_is_detected(self):
        self._git("mv", "core/view/src/design_ir_json.cpp", "outside.cpp")
        head = self._commit("rename out of slice")
        self.assertEqual(self._run(head), 1)


if __name__ == "__main__":
    unittest.main()
