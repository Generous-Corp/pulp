#!/usr/bin/env python3
"""Negative controls for the Vellum expansion watch."""

from __future__ import annotations

import copy
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).with_name("vellum_expansion_watch_check.py")
SPEC = importlib.util.spec_from_file_location("vellum_expansion_watch_check", SCRIPT)
assert SPEC and SPEC.loader
watch = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(watch)
ROOT = pathlib.Path(__file__).resolve().parents[2]


def acceptance() -> dict:
    return {
        "schema_version": 1,
        "kind": "authority-expansion-watch-acceptance",
        "acceptance_id": "full-design-import-render-v1-pulp-watch",
        "state": "accepted",
        "accepted_at": watch.EXPECTED_ACCEPTED_AT,
        "accepted_by": "@danielraffel",
        "pulp_repository": "Generous-Corp/pulp",
        "pulp_baseline_commit": "190c463a0b320f28420c9af177244af04ef84233",
        "counterpart": copy.deepcopy(watch.EXPECTED_COUNTERPART),
        "watch_scope": [
            {"id": family, "pulp_selectors": list(selectors)}
            for family, selectors in watch.EXPECTED_SCOPES.items()
        ],
        "retained_boundaries": list(watch.EXPECTED_RETAINED),
        "interim_maintenance": [{
            "id": "capture-primitives-unimplemented-in-vellum",
            "status": "pending-vellum-watch-acknowledgement",
            "expires_at_gate": "5A-P.3-independent-pixel-and-semantic-proof",
        }],
        "watch_event_directory": watch.EVENT_ROOT.as_posix(),
        "authority_effect": "none",
        "implementation_authority": (
            "forbidden-until-exact-boundary-acknowledged"
        ),
        "gates": dict(watch.EXPECTED_GATES),
    }


def event(*families: str, event_id: str = "20260811-render-watch-change") -> dict:
    return {
        "schema_version": 1,
        "kind": "authority-expansion-watch-change",
        "event_id": event_id,
        "created_at": "2026-08-11T02:30:19Z",
        "acceptance_id": "full-design-import-render-v1-pulp-watch",
        "acceptance_sha256": watch.EXPECTED_ACCEPTANCE_SHA256,
        "capability_families": sorted(families),
        "rationale": "Record the watched Pulp compatibility change without authority transfer.",
        "tests": ["vellum-expansion-watch"],
        "disposition": "watch-only-no-authority",
        "authority_effect": "none",
    }


def run(repo: pathlib.Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [*args],
        cwd=repo,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def write_json(path: pathlib.Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


class Tests(unittest.TestCase):
    def git_repo(self) -> tuple[tempfile.TemporaryDirectory[str], pathlib.Path]:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        repo = pathlib.Path(temporary.name)
        run(repo, "git", "init", "-q")
        run(repo, "git", "config", "user.name", "Watch Test")
        run(repo, "git", "config", "user.email", "watch@example.invalid")
        (repo / "seed.txt").write_text("seed\n", encoding="utf-8")
        run(repo, "git", "add", "seed.txt")
        run(repo, "git", "commit", "-q", "-m", "seed")
        return temporary, repo

    def commit(self, repo: pathlib.Path, message: str) -> str:
        run(repo, "git", "add", "-A")
        run(repo, "git", "commit", "-q", "-m", message)
        return run(repo, "git", "rev-parse", "HEAD").stdout.strip()

    def active_verify(
        self, repo: pathlib.Path, base: str, head: str, raw: bytes = b"accepted"
    ) -> dict:
        with mock.patch.object(
            watch,
            "load_acceptance",
            return_value=(watch.EXPECTED_SCOPES, raw),
        ):
            return watch.verify(repo, base, head)

    def test_workflows_run_watch_check_with_negative_controls(self) -> None:
        ordinary = (ROOT / ".github/workflows/vellum-freeze-check.yml").read_text()
        trusted = (ROOT / ".github/workflows/vellum-trusted-gate.yml").read_text()
        for workflow in (ordinary, trusted):
            self.assertIn("vellum_expansion_watch_check.py", workflow)
        self.assertIn("test_vellum_expansion_watch_check.py", ordinary)
        self.assertEqual(
            trusted.count(
                'if [ -f "$trusted_root/tools/scripts/vellum_expansion_watch_check.py" ]'
            ),
            2,
        )
        self.assertIn("vellum-trusted-expansion-watch.json", trusted)
        self.assertIn("vellum-merge-expansion-watch.json", trusted)
        self.assertEqual(
            trusted.count('test ! -e "$proposed_tree/.github/vellum-expansion-watch"'),
            2,
        )
        self.assertEqual(
            trusted.count(
                'test ! -e "$proposed_tree/.github/vellum-expansion-watch-events"'
            ),
            2,
        )

    def test_authority_surfaces_require_owner_review(self) -> None:
        codeowners = (ROOT / ".github/CODEOWNERS").read_text().splitlines()
        expected = {
            "/.github/vellum-expansion-watch/ @danielraffel",
            "/.github/vellum-expansion-watch-events/ @danielraffel",
            "/tools/scripts/vellum_expansion_watch_check.py @danielraffel",
            "/tools/scripts/test_vellum_expansion_watch_check.py @danielraffel",
        }
        self.assertTrue(expected <= set(codeowners))

    def test_absent_acceptance_is_clean_inactive_state(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        report = watch.verify(repo)
        self.assertEqual(report["status"], "pass", report["errors"])
        self.assertEqual(report["state"], "inactive")
        self.assertEqual(report["authority_effect"], "none")

    def test_exact_acceptance_structure_passes_without_authority(self) -> None:
        scopes = watch.validate_acceptance(acceptance())
        self.assertEqual(scopes, watch.EXPECTED_SCOPES)

    def test_acceptance_authority_claim_fails(self) -> None:
        value = acceptance()
        value["authority_effect"] = "transferred"
        with self.assertRaisesRegex(watch.WatchError, "authority_effect"):
            watch.validate_acceptance(value)

    def test_acceptance_counterpart_drift_fails(self) -> None:
        value = acceptance()
        value["counterpart"] = dict(value["counterpart"])
        value["counterpart"]["merge_commit"] = "0" * 40
        with self.assertRaisesRegex(watch.WatchError, "counterpart"):
            watch.validate_acceptance(value)

    def test_acceptance_scope_addendum_drift_fails(self) -> None:
        value = acceptance()
        value["counterpart"]["scope_addendum"]["merge_commit"] = "0" * 40
        with self.assertRaisesRegex(watch.WatchError, "counterpart"):
            watch.validate_acceptance(value)

    def test_acceptance_selector_drift_fails(self) -> None:
        value = acceptance()
        value["watch_scope"][0]["pulp_selectors"].pop()
        with self.assertRaisesRegex(watch.WatchError, "watch_scope"):
            watch.validate_acceptance(value)

    def test_globstar_matches_root_and_nested_paths(self) -> None:
        self.assertTrue(
            watch._matches(
                "tools/import-design/chromium_capture.py",
                "tools/import-design/**/*chrom*",
            )
        )
        self.assertTrue(
            watch._matches(
                "tools/import-design/runtime/chromium_capture.py",
                "tools/import-design/**/*chrom*",
            )
        )
        self.assertTrue(
            watch._matches(
                "tools/cli/import_design.cpp",
                "tools/cli/**/import_design*",
            )
        )

    def test_each_family_matches_representative_paths(self) -> None:
        cases = {
            "design-source-ingest": "tools/import-design/import.py",
            "chromium-authoring-frontend": "tools/import-design/chromium.py",
            "design-ir-contract": "core/view/src/design_import.cpp",
            "render-assets-and-backends": "core/render/backend.cpp",
            "visual-proof-harness": "tools/visual_compare.py",
            "design-output-and-packaging": "templates/native-design/app.js",
        }
        for family, path in cases.items():
            with self.subTest(family=family, path=path):
                self.assertIn(family, watch._affected(watch.EXPECTED_SCOPES, {path}))

    def test_addendum_closes_previously_omitted_surfaces(self) -> None:
        cases = {
            "design-source-ingest": "compat/rn.json",
            "chromium-authoring-frontend": "test/test_html_intake.cpp",
            "design-ir-contract": "core/view/src/jsx_lock.cpp",
            "render-assets-and-backends": "packages/pulp-react/src/index.ts",
            "visual-proof-harness": "tools/import-validation/layout_parity.py",
            "design-output-and-packaging": ".github/workflows/release-cli.yml",
        }
        for family, path in cases.items():
            with self.subTest(family=family, path=path):
                self.assertIn(family, watch._affected(watch.EXPECTED_SCOPES, {path}))

    def test_unrelated_audio_path_matches_no_watch_family(self) -> None:
        self.assertEqual(
            watch._affected(watch.EXPECTED_SCOPES, {"core/audio/processor.cpp"}),
            set(),
        )

    def test_boolean_schema_version_is_not_integer_one(self) -> None:
        value = acceptance()
        value["schema_version"] = True
        with self.assertRaisesRegex(watch.WatchError, "schema_version"):
            watch.validate_acceptance(value)

    def test_unknown_watch_artifact_fails_closed(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        (repo / watch.WATCH_ROOT).mkdir(parents=True)
        (repo / watch.WATCH_ROOT / "unknown.txt").write_text("authority\n")
        report = watch.verify(repo)
        self.assertEqual(report["status"], "fail")
        self.assertIn("artifact set differs", report["errors"][0])

    def test_malformed_acceptance_fails_before_schema_is_trusted(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        (repo / watch.README_PATH).parent.mkdir(parents=True)
        (repo / watch.README_PATH).write_text("wrong\n")
        (repo / watch.ACCEPTANCE_PATH).parent.mkdir(parents=True)
        (repo / watch.ACCEPTANCE_PATH).write_text(
            '{"schema_version":1,"schema_version":1}\n'
        )
        report = watch.verify(repo)
        self.assertEqual(report["status"], "fail")
        self.assertIn("immutable bytes differ", report["errors"][0])

    def test_watched_change_requires_event(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        path = repo / "core/render/new_backend.cpp"
        path.parent.mkdir(parents=True)
        path.write_text("render\n")
        head = self.commit(repo, "change render")
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "fail")
        self.assertIn("affected=['render-assets-and-backends']", report["errors"][0])

    def test_event_must_cover_exact_affected_families(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        path = repo / "core/view/src/design_import.cpp"
        path.parent.mkdir(parents=True)
        path.write_text("design\n")
        event_path = repo / watch.EVENT_ROOT / "20260811-render-watch-change.json"
        write_json(
            event_path,
            event("design-ir-contract", "render-assets-and-backends"),
        )
        head = self.commit(repo, "record design change")
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "pass", report["errors"])
        self.assertEqual(
            report["affected_families"],
            ["design-ir-contract", "render-assets-and-backends"],
        )

    def test_event_with_extra_family_fails(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        path = repo / "core/render/new_backend.cpp"
        path.parent.mkdir(parents=True)
        path.write_text("render\n")
        event_path = repo / watch.EVENT_ROOT / "20260811-render-watch-change.json"
        write_json(
            event_path,
            event("design-ir-contract", "render-assets-and-backends"),
        )
        head = self.commit(repo, "overclaim watched families")
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "fail")
        self.assertIn("coverage differs", report["errors"][0])

    def test_duplicate_family_claims_fail(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        path = repo / "core/render/new_backend.cpp"
        path.parent.mkdir(parents=True)
        path.write_text("render\n")
        for event_id in (
            "20260811-render-watch-change",
            "20260811-second-render-watch-change",
        ):
            write_json(
                repo / watch.EVENT_ROOT / f"{event_id}.json",
                event("render-assets-and-backends", event_id=event_id),
            )
        head = self.commit(repo, "duplicate watched family claims")
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "fail")
        self.assertIn("duplicate capability-family claims", report["errors"][0])

    def test_existing_event_is_append_only(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        event_path = repo / watch.EVENT_ROOT / "20260811-render-watch-change.json"
        write_json(event_path, event("render-assets-and-backends"))
        base = self.commit(repo, "seed event")
        value = event("render-assets-and-backends")
        value["rationale"] += " Modified."
        write_json(event_path, value)
        head = self.commit(repo, "modify event")
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "fail")
        self.assertIn("append-only", report["errors"][0])

    def test_event_cannot_claim_authority(self) -> None:
        value = event("render-assets-and-backends")
        value["authority_effect"] = "transferred"
        with self.assertRaisesRegex(watch.WatchError, "authority_effect"):
            watch.validate_event(
                value,
                ".github/vellum-expansion-watch-events/"
                "20260811-render-watch-change.json",
            )

    def test_acceptance_is_immutable_after_installation(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        path = repo / watch.ACCEPTANCE_PATH
        path.parent.mkdir(parents=True)
        path.write_bytes(b"old")
        base = self.commit(repo, "install acceptance")
        path.write_bytes(b"new")
        head = self.commit(repo, "rewrite acceptance")
        report = self.active_verify(repo, base, head, raw=b"new")
        self.assertEqual(report["status"], "fail")
        self.assertIn("immutable", report["errors"][0])

    def test_cli_writes_structured_inactive_report(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        output = repo / "report.json"
        completed = subprocess.run(
            [sys.executable, str(SCRIPT), "--repo", str(repo), "--output", str(output)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(output.read_text())["state"], "inactive")


if __name__ == "__main__":
    unittest.main()
