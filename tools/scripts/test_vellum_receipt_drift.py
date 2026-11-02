#!/usr/bin/env python3
"""Real-Git controls for the Vellum freeze check's receipt mode."""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

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
                          capture_output=True, text=True, env=env).stdout.strip()


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
            check=True, capture_output=True, text=True,
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

    def test_frozen_path_is_exempt(self):
        fixture = Fixture(frozen=["core/canvas/", "core/state/src/s.cpp"])
        try:
            fixture.write("core/state/src/s.cpp", "// edited\n")
            fixture.write("core/canvas/src/a.cpp", "// edited\n")
            fixture.write("core/canvas/src/new.cpp", "// new\n")
            self.assertEqual(fixture.check()["status"], "pass")
        finally:
            fixture.close()

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
                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("core/state/src/s.cpp", result.stderr)
        self.assertIn("vellum_inherits", result.stderr)


class CommittedPinTests(unittest.TestCase):
    def test_committed_pin_and_receipt_are_consistent(self):
        pin = drift.parse_pin((REPO / drift.PIN_PATH).read_bytes())
        receipt = drift.parse_receipt((REPO / pin.receipt_path).read_bytes(), pin)
        self.assertEqual(receipt.export_id, pin.export_id)
        self.assertGreater(len(receipt.exported), 0)
        self.assertGreater(len(receipt.source_dirs), 0)


if __name__ == "__main__":
    unittest.main()
