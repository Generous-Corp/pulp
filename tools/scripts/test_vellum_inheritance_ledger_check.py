#!/usr/bin/env python3
"""Negative controls for the sharded Vellum inheritance ledger checker."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import os
import shutil
import subprocess
import sys
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
        shutil.copytree(
            ROOT / checker.EXPANSION_EVENTS_DIR,
            self.root / checker.EXPANSION_EVENTS_DIR,
        )
        shutil.copytree(ROOT / checker.ENTRIES_DIR, self.root / checker.ENTRIES_DIR)
        shutil.copy2(ROOT / checker.FROZEN_LEDGER, self.root / checker.FROZEN_LEDGER)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_event(
        self,
        *,
        prefixed: bool = True,
        slices=("retained-ui-kernel",),
        expansion: bool = False,
        rationale: str | None = None,
        extra: dict | None = None,
    ) -> None:
        rationale = rationale or (
            checker.INHERITANCE_PREFIX + "; example." if prefixed else "Audio-only."
        )
        directory = checker.EXPANSION_EVENTS_DIR if expansion else checker.EVENTS_DIR
        event = {
            "schema_version": 1, "event_id": EVENT_ID, "kind": "change",
            "created_at": "2099-01-01T00:00:00Z", "slices": list(slices),
            "rationale": rationale, "tests": ["x"], "disposition": "pulp-only",
        }
        event.update(extra or {})
        (self.root / directory / f"{EVENT_ID}.json").write_text(
            json.dumps(event), encoding="utf-8",
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

    def test_inherited_by_vellum_expansion_event_without_entry_is_rejected(self) -> None:
        self.write_event(
            expansion=True,
            rationale="The generic fix is inherited by Vellum in the next extraction wave.",
        )
        errors = checker.check(self.root)
        self.assertTrue(any("has no entry" in error for error in errors), errors)

    def test_vellum_inherits_it_expansion_event_without_entry_is_rejected(self) -> None:
        self.write_event(
            expansion=True,
            rationale="Generic DesignIR hardening landed in Pulp; Vellum inherits it.",
        )
        errors = checker.check(self.root)
        self.assertTrue(any("has no entry" in error for error in errors), errors)

    def test_vellum_runtime_inherits_expansion_event_without_entry_is_rejected(self) -> None:
        self.write_event(
            expansion=True,
            rationale="Generic behavior that Vellum's JS UI runtime unit inherits.",
        )
        errors = checker.check(self.root)
        self.assertTrue(any("has no entry" in error for error in errors), errors)

    def test_advisory_inherit_wording_owes_no_entry(self) -> None:
        for rationale in (
            "This is not a shape Vellum should inherit.",
            "Vellum inherits nothing from this Pulp-only carve.",
            "These changes improve behavior inherited by Spectr.",
        ):
            with self.subTest(rationale=rationale):
                self.write_event(expansion=True, rationale=rationale)
                self.assertEqual(checker.check(self.root), [])

    def test_explicit_vellum_inherits_field_decides(self) -> None:
        self.write_event(expansion=True, prefixed=False, extra={"vellum_inherits": True})
        errors = checker.check(self.root)
        self.assertTrue(any("has no entry" in error for error in errors), errors)
        self.write_event(
            expansion=True,
            rationale="Vellum inherits it.",
            extra={"vellum_inherits": False},
        )
        self.assertEqual(checker.check(self.root), [])

    def test_backfilled_expansion_entries_are_required(self) -> None:
        for event_id in (
            "20261010-scripted-ui-mount-frame-bindings-watch",
            "20261010-scripted-ui-time-source-watch",
            "20261011-design-import-fail-closed-f4-watch",
            "20261011-design-ir-asset-path-lexical-watch",
            "20261011-design-ir-json-truncated-input-watch",
        ):
            with self.subTest(event_id=event_id):
                path = self.root / checker.ENTRIES_DIR / f"{event_id}.json"
                saved = path.read_bytes()
                path.unlink()
                errors = checker.check(self.root)
                path.write_bytes(saved)
                self.assertTrue(
                    any(event_id in error and "has no entry" in error for error in errors),
                    errors,
                )

    def test_expansion_capability_families_must_match_entry_slices(self) -> None:
        self.write_event(expansion=True)
        self.write_entry(slices=["different-slice"])
        errors = checker.check(self.root)
        self.assertTrue(any("slices" in error for error in errors), errors)

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
        self.assertTrue(any("no event in" in error for error in errors), errors)

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

    def test_existing_entry_cannot_be_deleted(self) -> None:
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
        path.unlink()
        git("commit", "-qam", "delete")
        errors = checker.check(self.root, "HEAD~1")
        self.assertTrue(any("append-only" in error for error in errors), errors)

    def test_ctest_runs_the_check_against_main(self) -> None:
        manifest = (ROOT / "test/cmake/quality_tests.cmake").read_text(encoding="utf-8")
        start = manifest.index("add_test(NAME vellum-inheritance-ledger\n")
        block = manifest[start:manifest.index(")", manifest.index("ledger_check.py", start))]
        self.assertIn("--base auto", block)

    def git(self, root: Path, *args: str) -> str:
        return subprocess.run(
            ["git", "-C", str(root), "-c", "maintenance.auto=false", "-c", "gc.auto=0",
             *args], check=True, text=True, encoding="utf-8",
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        ).stdout.strip()

    def commit_all(self, root: Path, message: str) -> str:
        self.git(root, "add", "-A")
        self.git(root, "-c", "user.email=t@example.com", "-c", "user.name=t",
                 "commit", "-qm", message)
        return self.git(root, "rev-parse", "HEAD")

    def run_ctest_command(self, root: Path, event: dict) -> subprocess.CompletedProcess:
        """The vellum-inheritance-ledger ctest command, as a CI job runs it."""
        event_path = root.parent / f"{root.name}-event.json"
        event_path.write_text(json.dumps(event), encoding="utf-8")
        env = dict(os.environ, GITHUB_ACTIONS="true", GITHUB_EVENT_PATH=str(event_path))
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(root), "--base", "auto"],
            text=True, encoding="utf-8", stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
        )

    def test_auto_base_uses_the_event_without_origin_main(self) -> None:
        self.git(self.root, "init", "-q")
        self.write_event()
        entry = self.write_entry()
        base = self.commit_all(self.root, "base")
        (self.root / "README.txt").write_text("unrelated\n", encoding="utf-8")
        self.commit_all(self.root, "head")
        self.assertEqual(
            subprocess.run(["git", "-C", str(self.root), "rev-parse", "--verify", "-q",
                            "origin/main"], stdout=subprocess.DEVNULL).returncode, 1)
        for event in ({"merge_group": {"base_sha": base}},
                      {"pull_request": {"base": {"sha": base}}}):
            with self.subTest(event=next(iter(event))):
                clean = self.run_ctest_command(self.root, event)
                self.assertEqual(clean.returncode, 0, clean.stderr)
        entry.unlink()
        self.commit_all(self.root, "delete an entry")
        deleted = self.run_ctest_command(self.root, {"merge_group": {"base_sha": base}})
        self.assertEqual(deleted.returncode, 1)
        self.assertIn("append-only (status D)", deleted.stderr)

    def test_auto_base_fetches_an_event_base_the_checkout_lacks(self) -> None:
        upstream = self.root
        self.git(upstream, "init", "-q")
        self.git(upstream, "config", "uploadpack.allowAnySHA1InWant", "true")
        self.write_event()
        entry = self.write_entry()
        base = self.commit_all(upstream, "base")
        entry.unlink()
        head = self.commit_all(upstream, "delete an entry")
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as clone_dir:
            clone = Path(clone_dir) / "work"
            clone.mkdir()
            self.git(clone, "init", "-q")
            self.git(clone, "remote", "add", "origin", upstream.as_uri())
            self.git(clone, "fetch", "-q", "--depth=1", "origin", head)
            self.git(clone, "checkout", "-q", head)
            self.assertNotEqual(
                subprocess.run(["git", "-C", str(clone), "cat-file", "-e", base],
                               stderr=subprocess.DEVNULL).returncode, 0)
            result = self.run_ctest_command(clone, {"merge_group": {"base_sha": base}})
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("append-only (status D)", result.stderr)

    def test_new_event_mentioning_vellum_inheritance_must_state_it(self) -> None:
        self.git(self.root, "init", "-q")
        self.commit_all(self.root, "base")
        self.write_event(expansion=True, rationale="Vellum will inherit this change.")
        self.commit_all(self.root, "add an ambiguous event")
        errors = checker.check(self.root, "HEAD~1")
        self.assertTrue(any("set \"vellum_inherits\"" in e for e in errors), errors)
        self.write_event(expansion=True, rationale="Vellum will inherit this change.",
                         extra={"vellum_inherits": False})
        self.git(self.root, "add", "-A")
        self.git(self.root, "-c", "user.email=t@example.com", "-c", "user.name=t",
                 "commit", "-q", "--amend", "--no-edit")
        self.assertEqual(checker.check(self.root, "HEAD~1"), [])

    def test_missing_base_ref_is_clear(self) -> None:
        errors = checker.check(self.root, "missing-base-ref")
        self.assertTrue(any("--base ref" in error and "unavailable" in error for error in errors), errors)


if __name__ == "__main__":
    unittest.main()
