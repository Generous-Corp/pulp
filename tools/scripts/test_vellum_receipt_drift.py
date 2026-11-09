#!/usr/bin/env python3
"""Real-Git controls for the Vellum freeze check's receipt mode."""

from __future__ import annotations

import base64
import contextlib
import hashlib
import io
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parent
REPO = SCRIPTS.parents[1]
sys.path.insert(0, str(SCRIPTS))

import vellum_receipt_drift as drift  # noqa: E402  (needs the path insert)

EXPORTED = [
    "core/canvas/include/pulp/canvas/a.hpp",
    "core/canvas/src/a.cpp",
    "core/state/src/s.cpp",
    "external/fonts/F.ttf",
]
PRODUCER = "tools/cmake/Embed.cmake"
# Present at the export commit but not exported, like another platform's source.
UNEXPORTED_AT_EXPORT = "core/canvas/src/other_platform.cpp"
FIXED_DATE = "2026-10-25T05:00:00Z"


def git_in(root: pathlib.Path, *args: str) -> str:
    env = {
        "GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "test@example.com",
        "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "test@example.com",
        "GIT_AUTHOR_DATE": FIXED_DATE, "GIT_COMMITTER_DATE": FIXED_DATE,
        "PATH": os.environ.get("PATH", ""), "HOME": str(root),
        "GIT_CONFIG_NOSYSTEM": "1",
    }
    return subprocess.run(["git", "-C", str(root), *args], check=True,
                          capture_output=True, text=True, encoding="utf-8", env=env).stdout.strip()


def make_export_commit(root: pathlib.Path) -> str:
    """The commit Vellum exported from. Fixed content and dates fix its SHA."""
    git_in(root, "init", "-q", "-b", "main")
    for path in [*EXPORTED, PRODUCER, UNEXPORTED_AT_EXPORT, "core/view/src/kept.cpp",
                 "docs/readme.md"]:
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(f"// {path}\n", encoding="utf-8")
    git_in(root, "add", "-A")
    git_in(root, "commit", "-q", "-m", "export source")
    return git_in(root, "rev-parse", "HEAD")


with tempfile.TemporaryDirectory() as _scratch:
    PULP_SHA = make_export_commit(pathlib.Path(_scratch))
EXPORT_ID = f"20261025-{PULP_SHA[:12]}"


def receipt_bytes(files: list[str] = EXPORTED) -> bytes:
    value = {
        "schema": drift.RECEIPT_SCHEMA,
        "export_id": EXPORT_ID,
        "pulp_repository": "Generous-Corp/pulp",
        "pulp_sha": PULP_SHA,
        "files": [{"pulp_path": path, "blob": "0" * 40} for path in files],
        "generated_inputs": [
            {"path": "@build/core/canvas/fonts.hpp", "producer_paths": [PRODUCER]}
        ],
    }
    return (json.dumps(value, indent=1, sort_keys=True) + "\n").encode()


def pin_value(receipt: bytes, frozen: list[str] | None = None) -> dict:
    return {
        "schema_version": 1,
        "framework_repository": "Generous-Corp/vellum",
        "export_id": EXPORT_ID,
        "receipt_source": f"provenance/mirror/receipts/{EXPORT_ID}.json",
        "receipt_path": f"{drift.PIN_DIRECTORY}/{EXPORT_ID}.json",
        "receipt_sha256": hashlib.sha256(receipt).hexdigest(),
        "frozen_paths": frozen or [],
    }


OWNERSHIP_MAP = {
    "slices": [
        {
            "id": "canvas-kernel",
            "state": "framework-authoritative-transferred",
            "paths": ["core/canvas/src/a.cpp"],
        },
        {"id": "state-kernel", "state": "excluded", "paths": ["core/state/src/s.cpp"]},
    ]
}


def export_event(event_id: str, paths: list[str], inherits, **overrides) -> dict:
    value = {
        "schema_version": 1,
        "event_id": event_id,
        "kind": "exported-path-change",
        "created_at": "2026-11-01T12:00:00Z",
        "receipt_export_id": EXPORT_ID,
        "paths": paths,
        "vellum_inherits": inherits,
        "rationale": "Records whether Vellum inherits this exported-path change.",
    }
    value.update(overrides)
    return value


class Fixture:
    """A repository with the pin at its base commit and one head commit on top."""

    def __init__(self, frozen: list[str] | None = None, with_pin: bool = True) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        export = make_export_commit(self.root)
        assert export == PULP_SHA, "the export commit must be reproducible"
        self.git("config", "user.email", "test@example.com")
        self.git("config", "user.name", "Test")
        self.write(drift.MAP_PATH, json.dumps(OWNERSHIP_MAP))
        if with_pin:
            receipt = receipt_bytes()
            self.write(f"{drift.PIN_DIRECTORY}/{EXPORT_ID}.json", receipt)
            self.write(drift.PIN_PATH, json.dumps(pin_value(receipt, frozen)))
        self.base = self.commit("base")

    def close(self) -> None:
        self.tmp.cleanup()

    def git(self, *args: str) -> str:
        return subprocess.run(
            ["git", "-C", str(self.root), *args],
            check=True, capture_output=True, text=True, encoding="utf-8",
        ).stdout.strip()

    def write(self, path: str, content) -> None:
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, (dict, list)):
            content = json.dumps(content, indent=2) + "\n"
        if isinstance(content, str):
            content = content.encode()
        target.write_bytes(content)

    def remove(self, path: str) -> None:
        self.git("rm", "-q", path)

    def commit(self, message: str) -> str:
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", message)
        return self.git("rev-parse", "HEAD")

    def check(self) -> dict:
        return drift.check(self.root, self.base, self.commit("head"))


class ReceiptDriftTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = Fixture()

    def tearDown(self) -> None:
        self.fixture.close()

    def assert_missing(self, report: dict, paths: list[str]) -> None:
        self.assertEqual(report["status"], "fail")
        self.assertEqual(report["missing"], paths)

    # --- what owes a decision -------------------------------------------------

    def test_unrelated_change_passes(self):
        # Negative control: the same instrument passes a change that touches no
        # exported path, so a failure below is about the exported path.
        self.fixture.write("core/view/src/kept.cpp", "// edited\n")
        self.fixture.write("docs/new.md", "new\n")
        report = self.fixture.check()
        self.assertEqual((report["status"], report["owed"]), ("pass", {}))

    def test_edit_of_exported_path_without_event_fails(self):
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        self.assert_missing(self.fixture.check(), ["core/state/src/s.cpp"])

    def test_deleting_an_exported_path_fails(self):
        self.fixture.remove("core/state/src/s.cpp")
        self.assert_missing(self.fixture.check(), ["core/state/src/s.cpp"])

    def test_renaming_an_exported_path_owes_both_sides(self):
        self.fixture.git("mv", "core/canvas/src/a.cpp", "core/canvas/src/b.cpp")
        self.assert_missing(self.fixture.check(),
                            ["core/canvas/src/a.cpp", "core/canvas/src/b.cpp"])

    def test_non_code_export_counts(self):
        self.fixture.write("external/fonts/F.ttf", b"\x00\x01")
        self.assert_missing(self.fixture.check(), ["external/fonts/F.ttf"])

    def test_generated_input_producer_counts(self):
        self.fixture.write(PRODUCER, "# edited\n")
        self.assert_missing(self.fixture.check(), [PRODUCER])

    def test_added_file_in_exported_source_directory_fails(self):
        self.fixture.write("core/canvas/src/new.cpp", "// new\n")
        self.assert_missing(self.fixture.check(), ["core/canvas/src/new.cpp"])

    def test_added_file_in_new_subdirectory_of_source_directory_fails(self):
        self.fixture.write("core/canvas/src/deep/new.cpp", "// new\n")
        self.assert_missing(self.fixture.check(), ["core/canvas/src/deep/new.cpp"])

    def test_added_file_beside_only_non_code_exports_passes(self):
        # external/fonts holds only a font, so it is not a source directory.
        self.fixture.write("external/fonts/G.ttf", b"\x00")
        self.assertEqual(self.fixture.check()["status"], "pass")

    def test_file_present_at_export_but_not_exported_passes(self):
        # Another platform's source sits beside exported files; the export did
        # not take it, so editing or deleting it owes nothing.
        self.fixture.write(UNEXPORTED_AT_EXPORT, "// edited\n")
        self.assertEqual(self.fixture.check()["status"], "pass")
        self.fixture.remove(UNEXPORTED_AT_EXPORT)
        self.assertEqual(self.fixture.check()["status"], "pass")

    def test_re_adding_an_unexported_file_in_source_directory_fails(self):
        # Adding a file under an exported source directory owes a decision even
        # when a file of that name existed at the export commit.
        self.fixture.remove(UNEXPORTED_AT_EXPORT)
        self.fixture.base = self.fixture.commit("removed")
        self.fixture.write(UNEXPORTED_AT_EXPORT, "// back\n")
        self.assert_missing(self.fixture.check(), [UNEXPORTED_AT_EXPORT])

    def test_editing_a_file_added_since_the_export_fails(self):
        # The next re-export picks the file up, so each later change to it owes
        # a decision too, not only the one that added it.
        self.fixture.write("core/canvas/src/new.cpp", "// new\n")
        self.fixture.base = self.fixture.commit("land it")
        self.fixture.write("core/canvas/src/new.cpp", "// edited\n")
        self.assert_missing(self.fixture.check(), ["core/canvas/src/new.cpp"])

    def test_deleting_a_file_added_since_the_export_fails(self):
        self.fixture.write("core/canvas/src/new.cpp", "// new\n")
        self.fixture.base = self.fixture.commit("land it")
        self.fixture.remove("core/canvas/src/new.cpp")
        self.assert_missing(self.fixture.check(), ["core/canvas/src/new.cpp"])

    def test_submodule_added_in_source_directory_fails_even_when_ignored(self):
        sha = self.fixture.git("rev-parse", "HEAD")
        self.fixture.git("update-index", "--add", "--cacheinfo",
                         f"160000,{sha},core/canvas/src/vendored")
        self.fixture.write(".gitmodules",
                           '[submodule "v"]\n\tpath = core/canvas/src/vendored\n'
                           "\turl = https://example.invalid/v.git\n\tignore = all\n")
        self.fixture.git("add", ".gitmodules")
        self.fixture.git("commit", "-q", "-m", "head")
        report = drift.check(self.fixture.root, self.fixture.base,
                             self.fixture.git("rev-parse", "HEAD"))
        self.assert_missing(report, ["core/canvas/src/vendored"])

    # --- what records the decision --------------------------------------------

    def test_export_event_with_decision_passes(self):
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        self.fixture.write(f"{drift.EXPORT_EVENT_DIRECTORY}/20261101-state-edit.json",
                           export_event("20261101-state-edit", ["core/state/src/s.cpp"], False))
        report = self.fixture.check()
        self.assertEqual(report["status"], "pass", report)

    def test_export_event_must_state_a_boolean(self):
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        self.fixture.write(f"{drift.EXPORT_EVENT_DIRECTORY}/20261101-state-edit.json",
                           export_event("20261101-state-edit", ["core/state/src/s.cpp"], "true"))
        with self.assertRaisesRegex(drift.ReceiptDriftError, "vellum_inherits"):
            self.fixture.check()

    def test_export_event_cannot_claim_an_untouched_path(self):
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        self.fixture.write(
            f"{drift.EXPORT_EVENT_DIRECTORY}/20261101-state-edit.json",
            export_event("20261101-state-edit",
                         ["core/canvas/src/a.cpp", "core/state/src/s.cpp"], True))
        with self.assertRaisesRegex(drift.ReceiptDriftError, "does not owe"):
            self.fixture.check()

    def test_export_event_must_name_the_pinned_receipt(self):
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        self.fixture.write(
            f"{drift.EXPORT_EVENT_DIRECTORY}/20261101-state-edit.json",
            export_event("20261101-state-edit", ["core/state/src/s.cpp"], True,
                         receipt_export_id="20261014-34d16417a460"))
        with self.assertRaisesRegex(drift.ReceiptDriftError, "pinned receipt"):
            self.fixture.check()

    def test_export_event_rejects_unknown_fields(self):
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        self.fixture.write(
            f"{drift.EXPORT_EVENT_DIRECTORY}/20261101-state-edit.json",
            export_event("20261101-state-edit", ["core/state/src/s.cpp"], True, frozen=True))
        with self.assertRaisesRegex(drift.ReceiptDriftError, "unexpected"):
            self.fixture.check()

    def test_export_event_rejects_duplicate_keys(self):
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        body = json.dumps(export_event("20261101-state-edit", ["core/state/src/s.cpp"], True))
        body = body[:-1] + ', "vellum_inherits": false}'
        self.fixture.write(f"{drift.EXPORT_EVENT_DIRECTORY}/20261101-state-edit.json", body)
        with self.assertRaisesRegex(drift.ReceiptDriftError, "duplicate"):
            self.fixture.check()

    def test_export_events_are_append_only(self):
        name = f"{drift.EXPORT_EVENT_DIRECTORY}/20261101-state-edit.json"
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        self.fixture.write(name, export_event("20261101-state-edit",
                                              ["core/state/src/s.cpp"], True))
        self.fixture.base = self.fixture.commit("landed")
        self.fixture.write(name, export_event("20261101-state-edit",
                                              ["core/state/src/s.cpp"], False))
        with self.assertRaisesRegex(drift.ReceiptDriftError, "append-only"):
            self.fixture.check()

    def test_event_from_an_earlier_change_does_not_cover(self):
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        self.fixture.write(f"{drift.EXPORT_EVENT_DIRECTORY}/20261101-state-edit.json",
                           export_event("20261101-state-edit", ["core/state/src/s.cpp"], True))
        self.fixture.base = self.fixture.commit("landed")
        self.fixture.write("core/state/src/s.cpp", "// edited again\n")
        self.assert_missing(self.fixture.check(), ["core/state/src/s.cpp"])

    def test_conflicting_decisions_fail(self):
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        for event_id, inherits in (("20261101-one", True), ("20261101-two", False)):
            self.fixture.write(f"{drift.EXPORT_EVENT_DIRECTORY}/{event_id}.json",
                               export_event(event_id, ["core/state/src/s.cpp"], inherits))
        report = self.fixture.check()
        self.assertEqual((report["status"], report["conflicting"]),
                         ("fail", ["core/state/src/s.cpp"]))

    def _land_range_conflict(self, path: str) -> str:
        """Two landed changes that disagree over one path, as a re-export range sees them."""
        self.fixture.write(path, "// added\n")
        self.fixture.write(f"{drift.EXPORT_EVENT_DIRECTORY}/20261101-claims-true.json",
                           export_event("20261101-claims-true", [path], True))
        self.fixture.commit("first change")
        self.fixture.write("core/canvas/include/pulp/canvas/a.hpp", "// edited\n")
        self.fixture.write(".github/vellum-expansion-watch-events/20261102-w.json",
                           {"capability_families": ["render-assets-and-backends"],
                            "vellum_inherits": False})
        return self.fixture.commit("second change")

    def test_range_conflict_between_landed_changes_fails(self):
        path = "core/canvas/include/pulp/canvas/added.hpp"
        self._land_range_conflict(path)
        report = self.fixture.check()
        self.assertEqual((report["status"], report["conflicting"]), ("fail", [path]))

    def test_superseding_export_event_settles_a_range_conflict(self):
        path = "core/canvas/include/pulp/canvas/added.hpp"
        landed = self._land_range_conflict(path)
        self.fixture.write(
            f"{drift.EXPORT_EVENT_DIRECTORY}/20261103-correction.json",
            export_event("20261103-correction", [path], False,
                         supersedes=["20261101-claims-true"]))
        head = self.fixture.commit("correct the first decision")
        # The whole range now carries one value for the path.
        report = drift.check(self.fixture.root, self.fixture.base, head)
        self.assertEqual((report["status"], report["conflicting"]), ("pass", []))
        # The correction alone touches no exported path and still passes.
        alone = drift.check(self.fixture.root, landed, head)
        self.assertEqual((alone["status"], alone["owed"]), ("pass", {}))

    def test_correction_cannot_claim_a_path_the_superseded_event_never_decided(self):
        path = "core/canvas/include/pulp/canvas/added.hpp"
        self._land_range_conflict(path)
        self.fixture.write(
            f"{drift.EXPORT_EVENT_DIRECTORY}/20261103-correction.json",
            export_event("20261103-correction", [path, "core/state/src/s.cpp"], False,
                         supersedes=["20261101-claims-true"]))
        with self.assertRaisesRegex(drift.ReceiptDriftError, "does not decide"):
            self.fixture.check()

    def test_correction_must_name_an_existing_export_event(self):
        path = "core/canvas/include/pulp/canvas/added.hpp"
        self._land_range_conflict(path)
        self.fixture.write(
            f"{drift.EXPORT_EVENT_DIRECTORY}/20261103-correction.json",
            export_event("20261103-correction", [path], False,
                         supersedes=["20261101-not-there"]))
        with self.assertRaisesRegex(drift.ReceiptDriftError, "not an export event"):
            self.fixture.check()

    def test_correction_cannot_supersede_itself(self):
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        self.fixture.write(
            f"{drift.EXPORT_EVENT_DIRECTORY}/20261103-correction.json",
            export_event("20261103-correction", ["core/state/src/s.cpp"], False,
                         supersedes=["20261103-correction"]))
        with self.assertRaisesRegex(drift.ReceiptDriftError, "other export events"):
            self.fixture.check()

    def test_change_event_covers_its_transferred_slice(self):
        self.fixture.write("core/canvas/src/a.cpp", "// edited\n")
        event = {"kind": "change", "slices": ["canvas-kernel"], "vellum_inherits": True}
        self.fixture.write(".github/vellum-change-events/20261101-canvas.json", event)
        self.assertEqual(self.fixture.check()["status"], "pass")

    def test_change_event_without_decision_does_not_cover(self):
        # The mislabelled-event case: a change event that names the slice but
        # does not say whether Vellum inherits the change.
        self.fixture.write("core/canvas/src/a.cpp", "// edited\n")
        event = {"kind": "change", "slices": ["canvas-kernel"], "disposition": "pulp-only"}
        self.fixture.write(".github/vellum-change-events/20261101-canvas.json", event)
        self.assert_missing(self.fixture.check(), ["core/canvas/src/a.cpp"])

    def test_change_event_decision_must_be_a_boolean(self):
        self.fixture.write("core/canvas/src/a.cpp", "// edited\n")
        event = {"kind": "change", "slices": ["canvas-kernel"], "vellum_inherits": "true"}
        self.fixture.write(".github/vellum-change-events/20261101-canvas.json", event)
        with self.assertRaisesRegex(drift.ReceiptDriftError, "vellum_inherits"):
            self.fixture.check()

    def test_edited_older_change_event_does_not_cover(self):
        # Only events this change adds are its statements; rewriting an
        # existing event to carry a decision records nothing.
        name = ".github/vellum-change-events/20261101-canvas.json"
        self.fixture.write(name, {"kind": "change", "slices": ["canvas-kernel"],
                                  "disposition": "pulp-only"})
        self.fixture.base = self.fixture.commit("older event")
        self.fixture.write("core/canvas/src/a.cpp", "// edited\n")
        self.fixture.write(name, {"kind": "change", "slices": ["canvas-kernel"],
                                  "vellum_inherits": True})
        self.assert_missing(self.fixture.check(), ["core/canvas/src/a.cpp"])

    def test_change_event_does_not_cover_paths_outside_its_slice(self):
        self.fixture.write("core/canvas/include/pulp/canvas/a.hpp", "// edited\n")
        event = {"kind": "change", "slices": ["canvas-kernel"], "vellum_inherits": True}
        self.fixture.write(".github/vellum-change-events/20261101-canvas.json", event)
        self.assert_missing(self.fixture.check(), ["core/canvas/include/pulp/canvas/a.hpp"])

    def test_change_event_slices_come_from_the_base_map(self):
        # A change cannot widen a slice in the same change to cover a path.
        self.fixture.write("core/canvas/include/pulp/canvas/a.hpp", "// edited\n")
        widened = json.loads(json.dumps(OWNERSHIP_MAP))
        widened["slices"][0]["paths"].append("core/canvas/include/pulp/canvas/a.hpp")
        self.fixture.write(drift.MAP_PATH, json.dumps(widened))
        event = {"kind": "change", "slices": ["canvas-kernel"], "vellum_inherits": True}
        self.fixture.write(".github/vellum-change-events/20261101-canvas.json", event)
        self.assert_missing(self.fixture.check(), ["core/canvas/include/pulp/canvas/a.hpp"])

    def test_watch_event_covers_its_family_selectors(self):
        self.fixture.write("core/canvas/include/pulp/canvas/a.hpp", "// edited\n")
        event = {"capability_families": ["render-assets-and-backends"],
                 "vellum_inherits": False}
        self.fixture.write(".github/vellum-expansion-watch-events/20261101-w.json", event)
        self.assertEqual(self.fixture.check()["status"], "pass")

    def test_watch_event_does_not_cover_unselected_paths(self):
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        event = {"capability_families": ["render-assets-and-backends"],
                 "vellum_inherits": True}
        self.fixture.write(".github/vellum-expansion-watch-events/20261101-w.json", event)
        self.assert_missing(self.fixture.check(), ["core/state/src/s.cpp"])

    # --- the pin ---------------------------------------------------------------

    def test_frozen_path_owes_no_decision_but_cannot_be_edited(self):
        fixture = Fixture(frozen=["core/canvas/", "core/state/src/s.cpp"])
        try:
            fixture.write("core/state/src/s.cpp", "// edited\n")
            fixture.write("core/canvas/src/a.cpp", "// edited\n")
            fixture.write("core/canvas/src/new.cpp", "// new\n")
            report = fixture.check()
            self.assertEqual(report["missing"], [])
            self.assertEqual(report["owed"], {})
            self.assertEqual(report["status"], "fail")
            self.assertEqual(report["frozen"], [
                "core/canvas/src/a.cpp", "core/canvas/src/new.cpp", "core/state/src/s.cpp",
            ])
        finally:
            fixture.close()

    def test_unfrozen_neighbour_still_owes_a_decision(self):
        fixture = Fixture(frozen=["core/canvas/src/a.cpp"])
        try:
            fixture.write("core/state/src/s.cpp", "// edited\n")
            report = fixture.check()
            self.assertEqual(report["frozen"], [])
            self.assert_missing(report, ["core/state/src/s.cpp"])
        finally:
            fixture.close()

    def test_pulp_only_change_event_does_not_unfreeze(self):
        fixture = Fixture(frozen=["core/canvas/src/a.cpp"])
        try:
            fixture.write("core/canvas/src/a.cpp", "// edited\n")
            event = {"kind": "change", "slices": ["canvas-kernel"],
                     "disposition": "pulp-only", "vellum_inherits": True}
            fixture.write(".github/vellum-change-events/20261102-canvas.json", event)
            report = fixture.check()
            self.assertEqual((report["status"], report["frozen"]),
                             ("fail", ["core/canvas/src/a.cpp"]))
        finally:
            fixture.close()

    def test_backport_or_emergency_change_event_unfreezes_its_slice(self):
        for disposition in ("framework-backport", "emergency-exception"):
            with self.subTest(disposition=disposition):
                fixture = Fixture(frozen=["core/canvas/src/a.cpp"])
                try:
                    fixture.write("core/canvas/src/a.cpp", "// edited\n")
                    event = {"kind": "change", "slices": ["canvas-kernel"],
                             "disposition": disposition}
                    fixture.write(".github/vellum-change-events/20261102-canvas.json", event)
                    report = fixture.check()
                    self.assertEqual((report["status"], report["frozen"]), ("pass", []))
                finally:
                    fixture.close()

    def test_unfreezing_event_covers_only_its_slice(self):
        fixture = Fixture(frozen=["core/canvas/", "core/state/src/s.cpp"])
        try:
            fixture.write("core/canvas/src/a.cpp", "// edited\n")
            fixture.write("core/state/src/s.cpp", "// edited\n")
            event = {"kind": "change", "slices": ["canvas-kernel"],
                     "disposition": "framework-backport"}
            fixture.write(".github/vellum-change-events/20261102-canvas.json", event)
            report = fixture.check()
            self.assertEqual((report["status"], report["frozen"]),
                             ("fail", ["core/state/src/s.cpp"]))
        finally:
            fixture.close()

    def test_freezing_takes_effect_after_the_change_that_freezes(self):
        # The pin is read at the base: freezing a path in the same change that
        # edits it neither exempts nor blocks that edit.
        self.fixture.write("core/canvas/src/a.cpp", "// edited\n")
        receipt = receipt_bytes()
        self.fixture.write(drift.PIN_PATH, pin_value(receipt, ["core/canvas/src/a.cpp"]))
        report = self.fixture.check()
        self.assertEqual(report["frozen"], [])
        self.assert_missing(report, ["core/canvas/src/a.cpp"])

    def test_pin_is_read_at_the_base(self):
        # Dropping the path from the receipt, or freezing it, in the same
        # change does not exempt the change.
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        receipt = receipt_bytes([path for path in EXPORTED if path != "core/state/src/s.cpp"])
        self.fixture.write(f"{drift.PIN_DIRECTORY}/{EXPORT_ID}.json", receipt)
        self.fixture.write(drift.PIN_PATH, pin_value(receipt, ["core/state/src/s.cpp"]))
        self.assert_missing(self.fixture.check(), ["core/state/src/s.cpp"])

    def test_pin_cannot_be_removed(self):
        self.fixture.remove(drift.PIN_PATH)
        with self.assertRaisesRegex(drift.ReceiptDriftError, "cannot be removed"):
            self.fixture.check()

    def test_receipt_digest_is_enforced(self):
        self.fixture.write(f"{drift.PIN_DIRECTORY}/{EXPORT_ID}.json", receipt_bytes() + b" ")
        self.fixture.base = self.fixture.commit("tampered receipt")
        with self.assertRaisesRegex(drift.ReceiptDriftError, "receipt_sha256"):
            self.fixture.check()

    def test_head_repin_is_validated(self):
        self.fixture.write(f"{drift.PIN_DIRECTORY}/{EXPORT_ID}.json", receipt_bytes() + b" ")
        with self.assertRaisesRegex(drift.ReceiptDriftError, "receipt_sha256"):
            self.fixture.check()

    def test_receipt_commit_must_be_pulp_history(self):
        receipt = receipt_bytes()
        fake = receipt.replace(PULP_SHA.encode(), (PULP_SHA[:12] + "f" * 28).encode())
        self.fixture.write(f"{drift.PIN_DIRECTORY}/{EXPORT_ID}.json", fake)
        self.fixture.write(drift.PIN_PATH, pin_value(fake))
        with self.assertRaisesRegex(drift.ReceiptDriftError, "not in this checkout"):
            self.fixture.check()

    def test_malformed_watch_event_fails_cleanly(self):
        self.fixture.write("core/canvas/include/pulp/canvas/a.hpp", "// edited\n")
        event = {"capability_families": [["render-assets-and-backends"]],
                 "vellum_inherits": True}
        self.fixture.write(".github/vellum-expansion-watch-events/20261101-w.json", event)
        with self.assertRaisesRegex(drift.ReceiptDriftError, "capability_families"):
            self.fixture.check()

    def test_malformed_change_event_fails_cleanly(self):
        self.fixture.write("core/canvas/src/a.cpp", "// edited\n")
        event = {"kind": "change", "slices": "canvas-kernel", "vellum_inherits": True}
        self.fixture.write(".github/vellum-change-events/20261101-canvas.json", event)
        with self.assertRaisesRegex(drift.ReceiptDriftError, "slices"):
            self.fixture.check()

    def test_no_pin_is_inactive(self):
        fixture = Fixture(with_pin=False)
        try:
            fixture.write("core/state/src/s.cpp", "// edited\n")
            self.assertEqual(fixture.check()["state"], "inactive")
        finally:
            fixture.close()

    def test_introducing_the_pin_is_judged_by_its_own_copy(self):
        fixture = Fixture(with_pin=False)
        try:
            receipt = receipt_bytes()
            fixture.write(f"{drift.PIN_DIRECTORY}/{EXPORT_ID}.json", receipt)
            fixture.write(drift.PIN_PATH, pin_value(receipt))
            fixture.write("core/state/src/s.cpp", "// edited\n")
            self.assert_missing(fixture.check(), ["core/state/src/s.cpp"])
        finally:
            fixture.close()

    def test_cli_exit_status(self):
        self.fixture.write("core/state/src/s.cpp", "// edited\n")
        head = self.fixture.commit("head")
        with tempfile.TemporaryDirectory() as out:
            result = subprocess.run(
                [sys.executable, str(SCRIPTS / "vellum_freeze_check.py"), "--receipt-mode",
                 "--repo", str(self.fixture.root), "--base", self.fixture.base,
                 "--head", head, "--output", str(pathlib.Path(out) / "report.json")],
                capture_output=True, text=True, encoding="utf-8", check=False)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("core/state/src/s.cpp", result.stderr)
        self.assertIn("vellum_inherits", result.stderr)


def transfer_value(paths: list[str] | None = None, **overrides) -> dict:
    value = {
        "schema_version": 1,
        "framework_repository": "Generous-Corp/vellum",
        "framework_pull_request": 257,
        "transfer_record": "provenance/framework/transfer.json",
        "transfer_record_sha256": "8" * 64,
        "framework_commit": FRAMEWORK_COMMIT,
        "export_id": EXPORT_ID,
        "pulp_sha": PULP_SHA,
        "transferred_paths": sorted(paths if paths is not None else EXPORTED),
    }
    value.update(overrides)
    return value


FRAMEWORK_COMMIT = "c" * 40
STATE = "core/state/src/s.cpp"  # exported, in no transferred slice, not frozen
CANVAS = "core/canvas/src/a.cpp"  # exported, in the canvas-kernel slice


class FrameworkTransferTests(unittest.TestCase):
    """After the transfer, a Pulp edit to a transferred path cannot claim Vellum inherits it."""

    def setUp(self) -> None:
        self.fixture = Fixture()

    def tearDown(self) -> None:
        self.fixture.close()

    def land_transfer(self, paths: list[str] | None = None) -> None:
        self.fixture.write(drift.TRANSFER_PATH, transfer_value(paths))
        self.fixture.base = self.fixture.commit("record the framework transfer")

    def edit_with_decision(self, path: str, inherits: bool) -> dict:
        self.fixture.write(path, "// edited\n")
        self.fixture.write(f"{drift.EXPORT_EVENT_DIRECTORY}/20261108-edit.json",
                           export_event("20261108-edit", [path], inherits))
        return self.fixture.check()

    def test_inherits_true_on_a_transferred_path_fails_after_the_transfer(self):
        self.land_transfer()
        report = self.edit_with_decision(STATE, True)
        self.assertEqual(report["status"], "fail", report)
        self.assertEqual(report["inherited_after_transfer"], [STATE])
        self.assertIn("vellum_inherits cannot be true", drift.describe_failure(report))

    def test_inherits_false_on_a_transferred_path_passes_after_the_transfer(self):
        self.land_transfer()
        report = self.edit_with_decision(STATE, False)
        self.assertEqual(report["status"], "pass", report)
        self.assertEqual(report["inherited_after_transfer"], [])

    def test_a_change_event_claiming_inheritance_fails_after_the_transfer(self):
        self.land_transfer()
        self.fixture.write(CANVAS, "// edited\n")
        self.fixture.write(".github/vellum-change-events/20261108-canvas.json",
                           {"kind": "change", "slices": ["canvas-kernel"], "vellum_inherits": True})
        report = self.fixture.check()
        self.assertEqual(report["inherited_after_transfer"], [CANVAS])
        self.assertEqual(report["status"], "fail")

    def test_before_the_transfer_behaviour_is_unchanged(self):
        report = self.edit_with_decision(STATE, True)
        self.assertEqual(report["status"], "pass", report)
        self.assertIsNone(report["transfer"])
        self.assertEqual(report["inherited_after_transfer"], [])

    def test_the_change_that_records_the_transfer_is_not_judged_by_it(self):
        self.fixture.write(drift.TRANSFER_PATH, transfer_value())
        self.assertEqual(self.edit_with_decision(STATE, True)["status"], "pass")

    def test_a_pinned_path_outside_the_transfer_is_refused_too(self):
        # The pinned receipt and the transfer's export can differ (Pulp's pin
        # lists paths the transfer dropped). Neither is re-exported, so the
        # refusal covers every path the pinned receipt makes owe a decision.
        self.land_transfer([path for path in EXPORTED if path != STATE])
        report = self.edit_with_decision(STATE, True)
        self.assertEqual(report["inherited_after_transfer"], [STATE])
        self.assertEqual(report["status"], "fail")

    def test_a_path_no_export_names_is_unaffected(self):
        self.land_transfer()
        self.fixture.write("docs/readme.md", "// edited\n")
        report = self.fixture.check()
        self.assertEqual(report["status"], "pass", report)
        self.assertEqual((report["owed"], report["inherited_after_transfer"]), ({}, []))

    def test_a_new_file_under_a_transferred_source_directory_is_refused(self):
        self.land_transfer()
        path = "core/canvas/src/brand_new.cpp"
        self.fixture.write(path, "// new\n")
        self.fixture.write(f"{drift.EXPORT_EVENT_DIRECTORY}/20261108-new.json",
                           export_event("20261108-new", [path], True))
        report = self.fixture.check()
        self.assertEqual(report["owed"], {path: "added in an exported source directory"})
        self.assertEqual(report["inherited_after_transfer"], [path])
        self.assertEqual(report["status"], "fail")

    def test_a_generated_input_producer_is_refused(self):
        self.land_transfer()
        self.fixture.write(PRODUCER, "# edited\n")
        self.fixture.write(f"{drift.EXPORT_EVENT_DIRECTORY}/20261108-producer.json",
                           export_event("20261108-producer", [PRODUCER], True))
        self.assertEqual(self.fixture.check()["inherited_after_transfer"], [PRODUCER])

    def test_an_unfreezing_event_on_a_frozen_path_cannot_claim_inheritance(self):
        for disposition in ("framework-backport", "emergency-exception"):
            for inherits, status in ((True, "fail"), (False, "pass")):
                with self.subTest(disposition=disposition, inherits=inherits):
                    fixture = Fixture(frozen=[CANVAS])
                    try:
                        fixture.write(drift.TRANSFER_PATH, transfer_value())
                        fixture.base = fixture.commit("record the framework transfer")
                        fixture.write(CANVAS, "// backported\n")
                        fixture.write(".github/vellum-change-events/20261108-canvas.json",
                                      {"kind": "change", "slices": ["canvas-kernel"],
                                       "disposition": disposition, "vellum_inherits": inherits})
                        report = fixture.check()
                        self.assertEqual(report["frozen"], [])
                        self.assertEqual(report["inherited_after_transfer"], [CANVAS] if inherits else [])
                        self.assertEqual(report["status"], status, report)
                    finally:
                        fixture.close()

    def test_a_watch_event_claiming_inheritance_on_a_frozen_path_is_refused(self):
        family, frozen = "design-ir-contract", "core/view/src/design_probe.cpp"
        self.assertTrue(any(drift.watch._matches(frozen, selector)  # control: the family selects it
                            for selector in drift.watch.EXPECTED_SCOPES[family]))
        for inherits, status in ((True, "fail"), (False, "pass")):
            with self.subTest(inherits=inherits):
                fixture = Fixture(frozen=[frozen])
                try:
                    fixture.write(drift.TRANSFER_PATH, transfer_value())
                    fixture.base = fixture.commit("record the framework transfer")
                    fixture.write(frozen, "// edited\n")
                    fixture.write(".github/vellum-change-events/20261108-backport.json",
                                  {"kind": "change", "slices": ["canvas-kernel"], "disposition": "framework-backport"})
                    fixture.write(".github/vellum-expansion-watch-events/20261108-watch.json",
                                  {"capability_families": [family], "vellum_inherits": inherits})
                    report = fixture.check()
                    self.assertEqual(report["inherited_after_transfer"], [frozen] if inherits else [])
                    if inherits:
                        self.assertEqual(report["status"], "fail")
                finally:
                    fixture.close()

    def test_every_failing_part_is_described(self):
        report = {"export_id": EXPORT_ID, "transfer": EXPORT_ID, "owed": {}, "missing": [],
                  "conflicting": [], "frozen": [CANVAS], "inherited_after_transfer": [STATE]}
        text = drift.describe_failure(report)
        self.assertIn("frozen in Pulp", text)
        self.assertIn("vellum_inherits cannot be true", text)
        self.assertNotIn("need an explicit vellum_inherits decision", text)
        report.update(missing=[STATE], owed={STATE: "exported"}, frozen=[])
        text = drift.describe_failure(report)
        self.assertIn("need an explicit vellum_inherits decision", text)
        self.assertIn("vellum_inherits cannot be true", text)

    def test_the_transfer_record_cannot_be_removed_or_edited(self):
        self.land_transfer()
        self.fixture.remove(drift.TRANSFER_PATH)
        with self.assertRaisesRegex(drift.ReceiptDriftError, "cannot be removed"):
            self.fixture.check()
        self.fixture.git("reset", "-q", "--hard", self.fixture.base)
        self.fixture.write(drift.TRANSFER_PATH, transfer_value([CANVAS]))
        with self.assertRaisesRegex(drift.ReceiptDriftError, "cannot be edited"):
            self.fixture.check()

    def test_a_malformed_transfer_record_fails_cleanly(self):
        for overrides in ({"framework_pull_request": "257"}, {"framework_pull_request": 0},
                          {"pulp_sha": "0" * 40}, {"transferred_paths": []}, {"extra": 1},
                          {"schema_version": 2}, {"framework_repository": "Generous-Corp/pulp"},
                          {"transfer_record": "provenance/framework/other.json"},
                          {"transfer_record_sha256": "8" * 63}, {"export_id": "2026-11-07"},
                          # Ends in the Pulp commit, so only the export_id format check refuses it.
                          {"export_id": f"2026110-{PULP_SHA[:12]}"},
                          {"framework_commit": "c" * 39}, {"framework_commit": "main"},
                          {"transferred_paths": ["../x"]},
                          {"transferred_paths": [EXPORTED[1], EXPORTED[0]]}):
            with self.subTest(overrides=overrides):
                self.fixture.write(drift.TRANSFER_PATH, transfer_value(**overrides))
                with self.assertRaises(drift.ReceiptDriftError):
                    self.fixture.check()

    def test_this_change_ships_the_rule_without_a_record(self):
        # The rule lands first; the record follows in a later change, after
        # Vellum's transfer merges. Until then receipt mode behaves as before.
        self.assertFalse((SCRIPTS.parents[1] / drift.TRANSFER_PATH).exists())
        self.assertIsNone(drift.transfer_in_effect(self.fixture.root, self.fixture.base, self.fixture.base))
        report = self.edit_with_decision(STATE, True)
        self.assertEqual((report["status"], report["transfer"], report["inherited_after_transfer"]),
                         ("pass", None, []))


class NoRecordIsInertTests(unittest.TestCase):
    """With no transfer record, every PR shape the existing tests cover is judged exactly as before."""

    def test_every_existing_receipt_mode_case_is_untouched(self):
        seen: list[dict] = []
        real_check = drift.check

        def check(repo, base, head):
            report = real_check(repo, base, head)
            seen.append(report)
            # The transfer machinery reads nothing and changes nothing.
            if report.get("transfer") is not None or report.get("inherited_after_transfer"):
                raise AssertionError(f"the transfer guard acted with no record: {report}")
            return report

        suite = unittest.TestSuite()
        for case in (ReceiptDriftTests, FrozenCliTests):
            suite.addTests(unittest.TestLoader().loadTestsFromTestCase(case))
        with mock.patch.object(drift, "check", check), \
                mock.patch.object(drift, "transfer_copy_problems",
                                  side_effect=AssertionError("no record, so nothing is fetched")):
            result = unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)
        self.assertTrue(result.wasSuccessful(), result.failures + result.errors)
        # Control: the existing cases ran through the wrapper (38 complete a
        # report today; the rest raise before one, or run the CLI in a subprocess).
        self.assertGreaterEqual(len(seen), 30)


class TransferSourceTests(unittest.TestCase):
    """The framework-transfer record must be Vellum main's transfer record."""

    SOURCE = "provenance/framework/transfer.json"

    def setUp(self) -> None:
        self.fixture = Fixture()
        self.vellum = json.dumps({"records": [{"pulp_path": p} for p in EXPORTED]}).encode()
        self.fixture.write(drift.TRANSFER_PATH,
                           transfer_value(transfer_record_sha256=hashlib.sha256(self.vellum).hexdigest()))
        self.head = self.fixture.commit("record the framework transfer")
        self.pulls = [{"number": 257, "base": {"ref": "main"}, "merged_at": "2026-11-09T05:00:00Z",
                       "merge_commit_sha": FRAMEWORK_COMMIT}]

    def tearDown(self) -> None:
        self.fixture.close()

    def served(self, data: bytes | None, status: str = "ahead", main: bytes | None = None, **overrides):
        """Vellum as the reader sees it: the record at FRAMEWORK_COMMIT, and main's position."""
        self.asked = []

        def fetch(path: str, expected=dict):
            self.asked.append(path)
            if data is None:
                raise OSError("unreachable")
            if path == f"repos/Generous-Corp/vellum/compare/{FRAMEWORK_COMMIT}...main?per_page=1":
                return {"status": status}
            if path == f"repos/Generous-Corp/vellum/commits/{FRAMEWORK_COMMIT}/pulls":
                return self.pulls
            self.assertEqual(path, f"repos/Generous-Corp/vellum/contents/{self.SOURCE}?ref={FRAMEWORK_COMMIT}")
            value = {"type": "file", "path": self.SOURCE, "encoding": "base64",
                     "content": base64.b64encode(data).decode()}
            value.update(overrides)
            return value
        return fetch

    def problems(self, fetch) -> list[str]:
        return drift.transfer_copy_problems(self.fixture.root, (self.fixture.base, self.head), fetch)

    def test_vellum_s_record_at_the_commit_matches(self):
        self.assertEqual(self.problems(self.served(self.vellum)), [])
        self.assertEqual(self.problems(self.served(self.vellum, status="identical")), [])

    def test_a_later_vellum_edit_on_main_does_not_break_the_record(self):
        # main's transfer.json has moved on; the record is read at its commit,
        # never at main, so it still matches.
        self.assertEqual(self.problems(self.served(self.vellum, main=self.vellum + b"\n")), [])
        self.assertFalse(any("ref=main" in path for path in self.asked), self.asked)

    def test_a_commit_main_does_not_contain_is_refused(self):
        for status in ("behind", "diverged"):
            with self.subTest(status=status):
                problems = self.problems(self.served(self.vellum, status=status))
                self.assertEqual(len(problems), 1)
                self.assertIn("is not on main", problems[0])

    def test_a_commit_without_the_record_is_refused(self):
        self.assertEqual(self.problems(self.served(self.vellum, type="dir")),
                         [f"Generous-Corp/vellum commit {FRAMEWORK_COMMIT[:12]} has no file {self.SOURCE}"])

    def test_other_bytes_at_the_commit_are_refused(self):
        problems = self.problems(self.served(self.vellum + b" "))
        self.assertEqual(len(problems), 1)
        self.assertIn("names transfer record", problems[0])

    def test_another_path_set_at_the_commit_is_refused(self):
        other = json.dumps({"records": [{"pulp_path": p} for p in EXPORTED[1:]]}).encode()
        self.fixture.write(drift.TRANSFER_PATH,
                           transfer_value(transfer_record_sha256=hashlib.sha256(other).hexdigest()))
        self.head = self.fixture.commit("record a transfer with another path set")
        problems = self.problems(self.served(other))
        self.assertEqual(len(problems), 1)
        self.assertIn("1 differ", problems[0])


    def test_an_unreachable_vellum_never_reads_as_a_match(self):
        with self.assertRaises(OSError):
            self.problems(self.served(None))

    def test_the_commit_must_be_the_merge_of_the_named_pull_request(self):
        refused = {
            "another pull request": [{"number": 999, "base": {"ref": "main"}, "merged_at": "2026-11-09T05:00:00Z",
                                      "merge_commit_sha": FRAMEWORK_COMMIT}],
            "not merged": [{"number": 257, "base": {"ref": "main"}, "merged_at": None,
                            "merge_commit_sha": FRAMEWORK_COMMIT}],
            "another base": [{"number": 257, "base": {"ref": "release"}, "merged_at": "2026-11-09T05:00:00Z",
                              "merge_commit_sha": FRAMEWORK_COMMIT}],
            "none": [],
            "not a list": {"message": "Not Found"},
            # A commit from the pull request's own branch is associated with it
            # too; only the merge commit is the pull request's merge.
            "branch commit": [{"number": 257, "base": {"ref": "main"}, "merged_at": "2026-11-09T05:00:00Z",
                               "merge_commit_sha": "d" * 40}],
        }
        for case, pulls in refused.items():
            with self.subTest(case=case):
                self.pulls = pulls
                problems = self.problems(self.served(self.vellum))
                self.assertEqual(problems, [f"Generous-Corp/vellum commit {FRAMEWORK_COMMIT[:12]} is not the merge "
                                            "of pull request #257 into main"])
        def failing(path: str, expected=dict):
            if path.endswith("/pulls"):
                raise OSError("pulls read failed")
            return self.served(self.vellum)(path, expected)
        self.pulls = [{"number": 257, "base": {"ref": "main"}, "merged_at": "2026-11-09T05:00:00Z",
                       "merge_commit_sha": FRAMEWORK_COMMIT}]
        with self.assertRaises(OSError):
            self.problems(failing)

    def test_an_unreadable_record_at_the_commit_is_refused(self):
        for case, data in {"undecodable": b"\xff\xfe not json", "no records": b'{"schema": "x"}'}.items():
            with self.subTest(case=case):
                problems = self.problems(self.served(data))
                self.assertEqual(len(problems), 1, problems)
                self.assertIn("is unreadable", problems[0])

    def test_a_non_dict_compare_result_is_refused(self):
        def fetch(path: str, expected=dict):
            if "/compare/" in path:
                return ["not", "a", "comparison"]
            return self.served(self.vellum)(path, expected)
        problems = self.problems(fetch)
        self.assertEqual(len(problems), 1)
        self.assertIn("is not on main (compare status None)", problems[0])

    def test_an_unreachable_framework_commit_fails_closed(self):
        # The compare read itself fails (a 404 for an unknown commit raises in
        # the reader): the error propagates and nothing reads as a match.
        def fetch(path: str, expected=dict):
            if "/compare/" in path:
                raise OSError("404 Not Found: no common ancestor or unknown commit")
            return self.served(self.vellum)(path, expected)
        with self.assertRaises(OSError):
            self.problems(fetch)
        # A reader that returns GitHub's error object instead of raising.
        def not_found(path: str, expected=dict):
            if "/compare/" in path:
                return {"message": "Not Found"}
            return self.served(self.vellum)(path, expected)
        problems = self.problems(not_found)
        self.assertEqual(len(problems), 1)
        self.assertIn("is not on main", problems[0])

    def test_a_commit_force_pushed_away_from_main_is_refused(self):
        # A force-push leaves the commit reachable by sha but not from main:
        # compare reports diverged, never ahead or identical.
        problems = self.problems(self.served(self.vellum, status="diverged"))
        self.assertEqual(len(problems), 1)
        self.assertIn("is not on main (compare status 'diverged')", problems[0])
        self.assertFalse(any("/contents/" in path for path in self.asked), self.asked)

    def test_trusted_reads_through_the_real_github_reader(self):
        # Only urlopen is replaced: the real _github_json and _public_github_json
        # parse each endpoint's real JSON shape (an object for compare and
        # contents, an array for commits/<sha>/pulls).
        import urllib.parse
        import vellum_freeze_check as freeze
        local = self.fixture.git("rev-parse", f"{self.fixture.base}:{drift.PIN_DIRECTORY}/{EXPORT_ID}.json")
        served_record = {"bytes": self.vellum}
        asked: list[str] = []

        def urlopen(request, timeout=None):
            url = urllib.parse.urlsplit(request.full_url)
            path, query = url.path, url.query
            asked.append(path + ("?" + query if query else ""))
            receipt = f"/repos/Generous-Corp/vellum/contents/provenance/mirror/receipts/{EXPORT_ID}.json"
            if path == receipt:
                body = {"type": "file", "path": receipt.split("/contents/")[1], "sha": local}
            elif path == f"/repos/Generous-Corp/vellum/compare/{FRAMEWORK_COMMIT}...main":
                body = {"status": "ahead", "ahead_by": 3, "behind_by": 0, "commits": []}
            elif path == f"/repos/Generous-Corp/vellum/commits/{FRAMEWORK_COMMIT}/pulls":
                body = [{"number": 257, "state": "closed", "base": {"ref": "main"},
                         "merged_at": "2026-11-09T05:00:00Z", "merge_commit_sha": FRAMEWORK_COMMIT}]
            elif path == "/repos/Generous-Corp/vellum/contents/provenance/framework/transfer.json":
                self.assertEqual(query, f"ref={FRAMEWORK_COMMIT}")
                encoded = base64.encodebytes(served_record["bytes"]).decode()  # GitHub wraps at 60 columns
                body = {"type": "file", "path": "provenance/framework/transfer.json",
                        "encoding": "base64", "content": encoded}
            else:
                raise AssertionError(f"unexpected read {path}?{query}")
            return io.BytesIO(json.dumps(body).encode())

        def run(base: str, head: str) -> int:
            with tempfile.TemporaryDirectory() as out, \
                    mock.patch.object(freeze.urllib.request, "urlopen", urlopen), \
                    mock.patch.dict(os.environ, {"VELLUM_READER_TOKEN": "t"}), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                return freeze.main(["--receipt-mode", "--repo", str(self.fixture.root), "--base", base,
                                    "--head", head, "--output", str(pathlib.Path(out) / "r.json")])

        # No record anywhere in the range: only the receipt copy is read.
        self.assertEqual(run(self.fixture.base, self.fixture.base), 0)
        self.assertFalse(any("framework" in path or "/pulls" in path for path in asked), asked)
        # The record matches Vellum's at its commit: every trusted read passes.
        asked.clear()
        self.assertEqual(run(self.fixture.base, self.head), 0)
        self.assertTrue(any(path.endswith("/pulls") for path in asked), asked)
        # Vellum's record at that commit differs: refused.
        served_record["bytes"] = self.vellum + b" "
        self.assertEqual(run(self.fixture.base, self.head), 1)

    def test_cli_refuses_a_record_whose_commit_main_does_not_contain(self):
        local = self.fixture.git("rev-parse", f"{self.fixture.base}:{drift.PIN_DIRECTORY}/{EXPORT_ID}.json")
        script = (
            "import base64, sys\n"
            f"sys.path.insert(0, {str(SCRIPTS)!r})\n"
            "import vellum_freeze_check as f\n"
            "local, record = sys.argv[1], sys.argv[2]\n"
            "def serve(url, token, **_):\n"
            "    if '/compare/' in url:\n"
            "        return {'status': 'diverged' if record == '-' else 'ahead'}\n"
            "    if url.endswith('/pulls'):\n"
            "        return [{'number': 257, 'base': {'ref': 'main'}, 'merged_at': '2026-11-09T05:00:00Z',\n"
            f"                 'merge_commit_sha': {FRAMEWORK_COMMIT!r}}}]\n"
            "    path = url.split('/contents/')[1].split('?')[0]\n"
            "    if path.startswith('provenance/mirror/'):\n"
            "        return {'type': 'file', 'path': path, 'sha': local}\n"
            "    return {'type': 'file', 'path': path, 'encoding': 'base64',\n"
            "            'content': base64.b64encode(open(record, 'rb').read()).decode()}\n"
            "f._github_json = serve\n"
            "sys.argv = [sys.argv[0]] + sys.argv[3:]\n"
            "raise SystemExit(f.main())\n"
        )
        with tempfile.TemporaryDirectory() as out:
            vellum = pathlib.Path(out) / "transfer.json"
            vellum.write_bytes(self.vellum)
            def run(record: str):
                env = dict(os.environ, VELLUM_READER_TOKEN="t")
                return subprocess.run(
                    [sys.executable, "-c", script, local, record, "--receipt-mode",
                     "--repo", str(self.fixture.root), "--base", self.fixture.base, "--head", self.head,
                     "--output", str(pathlib.Path(out) / "r.json")],
                    capture_output=True, text=True, encoding="utf-8", env=env, check=False)
            missing = run("-")
            self.assertEqual(missing.returncode, 1, missing.stdout + missing.stderr)
            self.assertIn("is not on main", missing.stderr)
            merged = run(str(vellum))
            self.assertEqual(merged.returncode, 0, merged.stdout + merged.stderr)

    def test_no_record_asks_nothing(self):
        self.assertEqual(drift.transfer_copy_problems(self.fixture.root, (self.fixture.base,),
                                                      lambda path: self.fail("fetched")), [])


class ReceiptSourceTests(unittest.TestCase):
    """The pinned receipt copy must be Vellum main's receipt, byte for byte."""

    SOURCE = f"provenance/mirror/receipts/{EXPORT_ID}.json"

    def setUp(self) -> None:
        self.fixture = Fixture()
        self.local = self.fixture.git("rev-parse", f"{self.fixture.base}:{drift.PIN_DIRECTORY}/{EXPORT_ID}.json")
        self.asked: list[str] = []

    def tearDown(self) -> None:
        self.fixture.close()

    def serve(self, sha: str, path: str | None = None):
        def fetch(endpoint: str):
            self.asked.append(endpoint)
            return {"type": "file", "path": path or self.SOURCE, "sha": sha}
        return fetch

    def problems(self, fetch, *commits: str) -> list[str]:
        return drift.receipt_copy_problems(self.fixture.root, commits or (self.fixture.base,), fetch)

    def test_matching_blob_passes_and_asks_vellum_main(self):
        self.assertEqual(self.problems(self.serve(self.local)), [])
        self.assertEqual(self.asked, [f"repos/Generous-Corp/vellum/contents/{self.SOURCE}?ref=main"])

    def test_a_copy_that_differs_from_vellum_fails(self):
        problems = self.problems(self.serve("f" * 40))
        self.assertEqual(len(problems), 1)
        self.assertIn("not Vellum's receipt", problems[0])

    def test_a_receipt_vellum_does_not_hold_fails(self):
        self.assertIn("has no file", self.problems(self.serve(self.local, path="elsewhere.json"))[0])

    def test_an_unreachable_vellum_is_not_a_match(self):
        def fetch(endpoint: str):
            raise drift.ReceiptDriftError("network down")
        with self.assertRaisesRegex(drift.ReceiptDriftError, "network down"):
            self.problems(fetch)

    def test_a_head_repin_is_checked_too(self):
        # A swapped copy in the change itself takes effect for later changes,
        # so the head pin's copy is compared as well as the base's.
        receipt = receipt_bytes(EXPORTED[:-1])
        self.fixture.write(f"{drift.PIN_DIRECTORY}/{EXPORT_ID}.json", receipt)
        self.fixture.write(drift.PIN_PATH, json.dumps(pin_value(receipt)))
        head = self.fixture.commit("swap the copy")
        problems = self.problems(self.serve(self.local), self.fixture.base, head)
        self.assertEqual(len(problems), 1)
        self.assertIn("not Vellum's receipt", problems[0])

    def test_cli_compares_only_with_a_reader_token(self):
        head = self.fixture.commit("head")
        script = (
            "import sys, runpy\n"
            f"sys.path.insert(0, {str(SCRIPTS)!r})\n"
            "import vellum_freeze_check as f\n"
            "served = sys.argv[1]\n"
            "f._github_json = lambda url, token, **_: {'type': 'file', 'path': url.split('/contents/')[1].split('?')[0], 'sha': served}\n"
            "sys.argv = [sys.argv[0]] + sys.argv[2:]\n"
            "raise SystemExit(f.main())\n"
        )
        def run(sha: str, token: str | None):
            env = {k: v for k, v in os.environ.items() if k != "VELLUM_READER_TOKEN"}
            if token:
                env["VELLUM_READER_TOKEN"] = token
            with tempfile.TemporaryDirectory() as out:
                return subprocess.run(
                    [sys.executable, "-c", script, sha, "--receipt-mode", "--repo", str(self.fixture.root),
                     "--base", self.fixture.base, "--head", head,
                     "--output", str(pathlib.Path(out) / "r.json")],
                    capture_output=True, text=True, encoding="utf-8", env=env, check=False)
        matched = run(self.local, "t")
        self.assertEqual(matched.returncode, 0, matched.stdout + matched.stderr)
        mismatch = run("f" * 40, "t")
        self.assertEqual(mismatch.returncode, 1, mismatch.stderr)
        self.assertIn("not Vellum's receipt", mismatch.stderr)
        # Without a token (the untrusted lane) nothing is fetched.
        self.assertEqual(run("f" * 40, None).returncode, 0)


class FrozenCliTests(unittest.TestCase):
    def test_cli_names_a_frozen_edit(self):
        fixture = Fixture(frozen=["core/canvas/src/a.cpp"])
        try:
            fixture.write("core/canvas/src/a.cpp", "// edited\n")
            head = fixture.commit("head")
            with tempfile.TemporaryDirectory() as out:
                result = subprocess.run(
                    [sys.executable, str(SCRIPTS / "vellum_freeze_check.py"), "--receipt-mode",
                     "--repo", str(fixture.root), "--base", fixture.base,
                     "--head", head, "--output", str(pathlib.Path(out) / "report.json")],
                    capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("frozen in Pulp", result.stderr)
            self.assertIn("core/canvas/src/a.cpp", result.stderr)
            self.assertIn("framework-backport", result.stderr)
        finally:
            fixture.close()


OWNER_UNFREEZES_PATH = ".github/vellum-export-receipt/owner-unfreezes.json"
OWNER_UNFREEZES_SCHEMA = "pulp.vellum.owner-unfreezes.v2"
# The freeze owner, as GitHub spells the login (the ownership map writes it
# "@danielraffel").
OWNER_LOGIN = "danielraffel"
APPROVAL_URL_RE = re.compile(
    r"^https://github\.com/Generous-Corp/pulp/pull/[1-9][0-9]*#issuecomment-[1-9][0-9]*$"
)


def owner_unfrozen_paths(raw: bytes, transferred: set[str]) -> set[str]:
    """Validate the owner-controlled unfreeze record and return its exact paths.

    The record is the only source of pin removals the consistency test accepts.
    Each entry names one exact transferred-slice path, the URL of the owner's
    approval comment on a Pulp pull request, and ``approved_by``, the comment's
    author, which must be the freeze owner. Anything else raises ValueError, so
    a pin deletion without a recorded approval cannot pass. The comment's author
    is recorded evidence: this offline check does not query GitHub for it.
    """
    value = json.loads(raw)
    if not isinstance(value, dict) or value.get("schema") != OWNER_UNFREEZES_SCHEMA:
        raise ValueError("owner-unfreezes: wrong schema")
    paths: set[str] = set()
    for index, row in enumerate(value.get("unfreezes", [])):
        where = f"owner-unfreezes[{index}]"
        if not isinstance(row, dict) or set(row) != {"path", "approval_url", "approved_by"}:
            raise ValueError(f"{where}: needs exactly path, approval_url, approved_by")
        if row["approved_by"] != OWNER_LOGIN:
            raise ValueError(f"{where}: approved_by must be the freeze owner {OWNER_LOGIN}")
        if not isinstance(row["approval_url"], str) or not APPROVAL_URL_RE.fullmatch(
            row["approval_url"]
        ):
            raise ValueError(f"{where}: approval_url must be a Pulp pull request comment URL")
        if row["path"] not in transferred:
            raise ValueError(f"{where}: path is not an exact transferred-slice path")
        if row["path"] in paths:
            raise ValueError(f"{where}: repeated path")
        paths.add(row["path"])
    return paths


class OwnerUnfreezeRecordTests(unittest.TestCase):
    TRANSFERRED = {"a/x.hpp", "a/y.hpp"}

    def record(self, **row):
        base = {"path": "a/x.hpp",
                "approval_url": "https://github.com/Generous-Corp/pulp/pull/1#issuecomment-2",
                "approved_by": OWNER_LOGIN}
        base.update(row)
        return json.dumps({"schema": OWNER_UNFREEZES_SCHEMA, "unfreezes": [base]}).encode()

    def test_recorded_path_is_accepted(self):
        self.assertEqual(owner_unfrozen_paths(self.record(), self.TRANSFERRED), {"a/x.hpp"})

    def test_unrecorded_pin_deletion_fails_consistency(self):
        # A path missing from the pin but absent from the record is not accepted.
        recorded = owner_unfrozen_paths(self.record(), self.TRANSFERRED)
        pin_after_unrecorded_deletion = ["a/x.hpp"]  # a/y.hpp deleted, no record
        expected = [p for p in sorted(self.TRANSFERRED) if p not in recorded]
        self.assertNotEqual(pin_after_unrecorded_deletion, expected)

    def test_record_rejects_forged_entries(self):
        for bad in (self.record(path="a/z.hpp"), self.record(path="a"),
                    self.record(approved_by="someone-else"),
                    self.record(approved_by="@danielraffel"),
                    self.record(approval_url="trust me"),
                    self.record(approval_url="https://github.com/Generous-Corp/pulp/pull/1"),
                    self.record(approval_url="https://github.com/other/pulp/pull/1#issuecomment-2"),
                    self.record(approval_url="https://github.com/Generous-Corp/pulp/pull/1#issuecomment-2x")):
            with self.assertRaises(ValueError):
                owner_unfrozen_paths(bad, self.TRANSFERRED)


STALE_ID = "20261014-34d16417a460"
EVENTS = drift.EXPORT_EVENT_DIRECTORY


class RepinEventsTests(unittest.TestCase):
    """`--repin-events` repairs what a re-pin or a merged parent event breaks.

    Each case first proves the branch fails receipt mode the way CI judges it
    (live main against the branch merged into it), then that the rewrite makes
    the same judgement pass.
    """

    def setUp(self) -> None:
        self.fixture = Fixture()
        self.git = self.fixture.git

    def tearDown(self) -> None:
        self.fixture.close()

    def event_bytes(self, event_id: str) -> bytes:
        return (self.fixture.root / EVENTS / f"{event_id}.json").read_bytes()

    def branch(self, name: str, start: str = "main") -> None:
        self.git("checkout", "-q", "-b", name, start)

    def ci_check(self) -> dict:
        """Receipt mode as the freeze lanes run it: base live main, head the merge."""
        main = self.git("rev-parse", "main")
        head = self.git("rev-parse", "HEAD")
        tree = self.git("merge-tree", "--write-tree", main, head).splitlines()[0]
        candidate = self.git("commit-tree", tree, "-p", main, "-p", head, "-m", "candidate")
        return drift.check(self.fixture.root, main, candidate)

    def repin(self) -> tuple[list[str], list[str]]:
        return drift.repin_events(self.fixture.root, "main")

    def test_a_stale_receipt_id_is_rewritten_to_the_pin_on_main(self):
        self.branch("topic")
        self.fixture.write(STATE, "// edited\n")
        self.fixture.write(f"{EVENTS}/20261101-state-edit.json",
                           export_event("20261101-state-edit", [STATE], False,
                                        receipt_export_id=STALE_ID))
        self.fixture.commit("edit with an event naming the previous receipt")
        # main moves on after the fork, as it does in the field.
        self.git("checkout", "-q", "main")
        self.fixture.write("docs/later.md", "later\n")
        self.fixture.commit("unrelated main change")
        self.git("checkout", "-q", "topic")
        with self.assertRaises(drift.ReceiptDriftError) as raised:
            self.ci_check()
        self.assertIn(f"must name the pinned receipt {EXPORT_ID}", str(raised.exception))
        self.assertIn(drift.REPIN_COMMAND, str(raised.exception))
        self.assertIn("committed HEAD", str(raised.exception))

        changed, notes = self.repin()
        self.assertEqual(notes, [])
        self.assertEqual(changed, [f"{EVENTS}/20261101-state-edit.json: receipt_export_id "
                                   f"{STALE_ID} -> {EXPORT_ID}"])
        event = json.loads(self.event_bytes("20261101-state-edit"))
        self.assertEqual(event["receipt_export_id"], EXPORT_ID)
        self.assertEqual(event["paths"], [STATE])
        self.fixture.commit("re-pin")
        self.assertEqual(self.ci_check()["status"], "pass")
        # Idempotent: a second run finds nothing and rewrites nothing.
        before = self.event_bytes("20261101-state-edit")
        self.assertEqual(self.repin(), ([], []))
        self.assertEqual(self.event_bytes("20261101-state-edit"), before)

    def test_a_stacked_event_drops_paths_its_merged_parent_already_decides(self):
        parent_event = f"{EVENTS}/20261101-parent.json"
        self.branch("parent")
        self.fixture.write(STATE, "// parent edit\n")
        self.fixture.write(parent_event, export_event("20261101-parent", [STATE], False))
        self.fixture.commit("parent")
        self.branch("child", "parent")
        self.fixture.write(EXPORTED[0], "// child edit\n")
        self.fixture.write(f"{EVENTS}/20261102-child.json",
                           export_event("20261102-child", [EXPORTED[0], STATE], True,
                                        receipt_export_id=STALE_ID))
        self.fixture.commit("child")
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--no-ff", "-m", "land parent", "parent")
        self.git("checkout", "-q", "child")
        main_events = {name: self.git("show", f"main:{name}")
                       for name in drift._export_event_paths(self.fixture.root, "main")}
        self.assertEqual(list(main_events), [parent_event])
        with self.assertRaises(drift.ReceiptDriftError):
            self.ci_check()

        changed, notes = self.repin()
        self.assertEqual(notes, [])
        self.assertEqual(changed, [
            f"{EVENTS}/20261102-child.json: receipt_export_id {STALE_ID} -> {EXPORT_ID}",
            f"{EVENTS}/20261102-child.json: dropped {STATE} (already decided on main by "
            f"{parent_event})",
        ])
        self.assertEqual(json.loads(self.event_bytes("20261102-child"))["paths"],
                         [EXPORTED[0]])
        self.fixture.commit("re-pin")
        self.assertEqual(self.ci_check()["status"], "pass")
        self.assertEqual(self.repin(), ([], []))
        for name, text in main_events.items():
            self.assertEqual(self.git("show", f"HEAD:{name}"), text, name)

    def test_the_stray_path_error_names_the_merged_event_that_decides_it(self):
        parent_event = f"{EVENTS}/20261101-parent.json"
        self.branch("parent")
        self.fixture.write(STATE, "// parent edit\n")
        self.fixture.write(parent_event, export_event("20261101-parent", [STATE], False))
        self.fixture.commit("parent")
        self.branch("child", "parent")
        self.fixture.write(EXPORTED[0], "// child edit\n")
        self.fixture.write(f"{EVENTS}/20261102-child.json",
                           export_event("20261102-child", [EXPORTED[0], STATE], True))
        self.fixture.commit("child")
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--no-ff", "-m", "land parent", "parent")
        self.git("checkout", "-q", "child")
        with self.assertRaises(drift.ReceiptDriftError) as raised:
            self.ci_check()
        message = str(raised.exception)
        self.assertIn("does not owe a decision for: " + STATE, message)
        self.assertIn(f"{parent_event}: {STATE}", message)
        self.assertIn(drift.REPIN_COMMAND, message)

        changed, notes = self.repin()
        self.assertEqual(notes, [])
        self.assertEqual(changed, [f"{EVENTS}/20261102-child.json: dropped {STATE} (already "
                                   f"decided on main by {parent_event})"])
        self.fixture.commit("re-pin")
        self.assertEqual(self.ci_check()["status"], "pass")

    def test_a_path_the_branch_changes_again_is_kept(self):
        # main decided STATE once; this branch edits it again, so it still owes it.
        self.fixture.write(STATE, "// landed edit\n")
        self.fixture.write(f"{EVENTS}/20261101-landed.json",
                           export_event("20261101-landed", [STATE], False))
        self.fixture.commit("landed")
        self.branch("topic")
        self.fixture.write(STATE, "// edited again\n")
        self.fixture.write(f"{EVENTS}/20261102-again.json",
                           export_event("20261102-again", [STATE], True))
        self.fixture.commit("again")
        before = self.event_bytes("20261102-again")
        self.assertEqual(self.repin(), ([], []))
        self.assertEqual(self.event_bytes("20261102-again"), before)
        self.assertEqual(self.ci_check()["status"], "pass")

    def test_an_event_left_with_no_path_is_deleted(self):
        parent_event = f"{EVENTS}/20261101-parent.json"
        self.branch("parent")
        self.fixture.write(STATE, "// parent edit\n")
        self.fixture.write(parent_event, export_event("20261101-parent", [STATE], False))
        self.fixture.commit("parent")
        self.branch("child", "parent")
        self.fixture.write(f"{EVENTS}/20261102-child.json",
                           export_event("20261102-child", [STATE], False))
        self.fixture.write("docs/child.md", "child\n")
        self.fixture.commit("child")
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--no-ff", "-m", "land parent", "parent")
        self.git("checkout", "-q", "child")
        changed, _ = self.repin()
        self.assertIn(f"{EVENTS}/20261102-child.json: deleted, every path it listed is "
                      "already decided", changed)
        self.assertFalse((self.fixture.root / EVENTS / "20261102-child.json").exists())
        self.fixture.commit("re-pin")
        self.assertEqual(self.ci_check()["status"], "pass")

    def test_a_clean_branch_is_a_no_op(self):
        self.branch("topic")
        self.fixture.write(STATE, "// edited\n")
        self.fixture.write(f"{EVENTS}/20261101-state-edit.json",
                           export_event("20261101-state-edit", [STATE], False))
        self.fixture.commit("edit")
        before = self.event_bytes("20261101-state-edit")
        self.assertEqual(self.repin(), ([], []))
        self.assertEqual(self.event_bytes("20261101-state-edit"), before)
        self.assertEqual(self.git("status", "--porcelain"), "")

    def test_events_already_on_main_are_never_modified(self):
        # A landed event names the previous receipt; it stays as it landed.
        self.fixture.write(STATE, "// landed\n")
        landed = f"{EVENTS}/20261020-landed.json"
        self.fixture.write(landed, export_event("20261020-landed", [STATE], False,
                                                receipt_export_id=STALE_ID))
        self.fixture.commit("landed under the previous pin")
        landed_bytes = (self.fixture.root / landed).read_bytes()
        self.branch("topic")
        self.fixture.write(EXPORTED[0], "// edited\n")
        self.fixture.write(f"{EVENTS}/20261101-mine.json",
                           export_event("20261101-mine", [EXPORTED[0]], False,
                                        receipt_export_id=STALE_ID))
        self.fixture.commit("mine")
        changed, _ = self.repin()
        self.assertEqual(changed, [f"{EVENTS}/20261101-mine.json: receipt_export_id "
                                   f"{STALE_ID} -> {EXPORT_ID}"])
        self.assertEqual((self.fixture.root / landed).read_bytes(), landed_bytes)

    def test_a_parent_event_carried_by_a_squash_landing_is_not_touched(self):
        # The parent landed as a squash, so its event file exists on main and
        # in this branch's own history; it belongs to main, not to the branch.
        parent_event = f"{EVENTS}/20261101-parent.json"
        self.branch("parent")
        self.fixture.write(STATE, "// parent edit\n")
        self.fixture.write(parent_event, export_event("20261101-parent", [STATE], False,
                                                      receipt_export_id=STALE_ID))
        self.fixture.commit("parent")
        parent_bytes = (self.fixture.root / parent_event).read_bytes()
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--squash", "parent")
        self.fixture.commit("squash parent")
        self.git("checkout", "-q", "parent")
        self.assertEqual(self.repin(), ([], []))
        self.assertEqual((self.fixture.root / parent_event).read_bytes(), parent_bytes)

    def test_the_command_line_repins_and_reports(self):
        self.branch("topic")
        self.fixture.write(STATE, "// edited\n")
        self.fixture.write(f"{EVENTS}/20261101-state-edit.json",
                           export_event("20261101-state-edit", [STATE], False,
                                        receipt_export_id=STALE_ID))
        self.fixture.commit("edit")

        def run() -> subprocess.CompletedProcess:
            return subprocess.run(
                [sys.executable, str(SCRIPTS / "vellum_receipt_drift.py"), "--repin-events",
                 "--repo", str(self.fixture.root), "--base-branch", "main"],
                capture_output=True, text=True, encoding="utf-8", check=False)

        first = run()
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertIn(f"{STALE_ID} -> {EXPORT_ID}", first.stdout)
        self.assertIn("committed HEAD", first.stdout)
        second = run()
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn("nothing to change", second.stdout)


class CommittedPinTests(unittest.TestCase):
    def test_committed_pin_and_receipt_are_consistent(self):
        pin = drift.parse_pin((REPO / drift.PIN_PATH).read_bytes())
        receipt = drift.parse_receipt((REPO / pin.receipt_path).read_bytes(), pin)
        self.assertEqual(receipt.export_id, pin.export_id)
        self.assertGreater(len(receipt.exported), 0)
        self.assertGreater(len(receipt.source_dirs), 0)

    def test_committed_pin_freezes_exactly_the_transferred_slices(self):
        pin = drift.parse_pin((REPO / drift.PIN_PATH).read_bytes())
        mapping = json.loads((REPO / drift.MAP_PATH).read_text(encoding="utf-8"))
        transferred = sorted({
            path
            for item in mapping["slices"]
            if item["state"] == "framework-authoritative-transferred"
            for path in item["paths"]
        })
        unfrozen = owner_unfrozen_paths((REPO / OWNER_UNFREEZES_PATH).read_bytes(),
                                        set(transferred))
        self.assertEqual(list(pin.frozen_paths),
                         [path for path in transferred if path not in unfrozen])


if __name__ == "__main__":
    unittest.main()
