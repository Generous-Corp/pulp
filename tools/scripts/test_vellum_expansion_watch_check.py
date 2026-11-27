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


def refresh_event() -> dict:
    return {
        "schema_version": 1,
        "kind": "authority-expansion-watch-refresh",
        "event_id": "20260813-exact-boundary-audit-refresh",
        "created_at": "2026-08-13T16:02:12Z",
        "acceptance_id": "full-design-import-render-v1-pulp-watch",
        "acceptance_sha256": watch.EXPECTED_ACCEPTANCE_SHA256,
        "capability_families": [],
        "rationale": (
            "Record the current-main exact-boundary audit refresh without "
            "rewriting the immutable acceptance or transferring authority."
        ),
        "tests": ["vellum-expansion-watch"],
        "disposition": "watch-only-no-authority",
        "authority_effect": "none",
        "refresh": {
            "audited_at": "2026-08-13T16:02:12Z",
            "pulp_main_commit": "f856cbfad79554025d6c70f604f6af835d55ad3a",
            "open_pr_audit_complete": True,
            "open_pr_rows": [],
            "open_vellum_overlap_count": 0,
        },
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

    def test_authority_surfaces_listed_in_codeowners(self) -> None:
        # Listed without an owner so GitHub requests no review; activation
        # restores the owner (docs/contracts/vellum-activation-readiness.md).
        codeowners = (ROOT / ".github/CODEOWNERS").read_text().splitlines()
        patterns = {line.split()[0] for line in codeowners if line.strip()
                    and not line.startswith("#")}
        expected = {
            "/.github/vellum-expansion-watch/",
            "/.github/vellum-expansion-watch-events/",
            "/tools/scripts/vellum_expansion_watch_check.py",
            "/tools/scripts/test_vellum_expansion_watch_check.py",
        }
        self.assertTrue(expected <= patterns)

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

    def test_refresh_event_is_append_only_evidence_without_scope_claim(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        write_json(
            repo / watch.EVENT_ROOT / "20260813-exact-boundary-audit-refresh.json",
            refresh_event(),
        )
        head = self.commit(repo, "record exact-boundary audit refresh")
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "pass", report["errors"])
        self.assertEqual(report["affected_families"], [])

    def test_unknown_event_kind_fails_closed(self) -> None:
        value = event("render-assets-and-backends")
        value["kind"] = "authority-expansion-watch-unknown"
        with self.assertRaisesRegex(watch.WatchError, "kind differs"):
            watch.validate_event(
                value,
                ".github/vellum-expansion-watch-events/"
                "20260811-render-watch-change.json",
            )

    def test_vellum_inherits_is_an_optional_boolean(self) -> None:
        name = ".github/vellum-expansion-watch-events/20260811-render-watch-change.json"
        for flag in (True, False):
            value = event("render-assets-and-backends")
            value["vellum_inherits"] = flag
            watch.validate_event(value, name)
        value = event("render-assets-and-backends")
        value["vellum_inherits"] = "yes"
        with self.assertRaisesRegex(watch.WatchError, "vellum_inherits must be true or false"):
            watch.validate_event(value, name)

    def test_refresh_overlap_count_requires_integer_zero(self) -> None:
        value = refresh_event()
        value["refresh"]["open_vellum_overlap_count"] = False
        with self.assertRaisesRegex(watch.WatchError, "expected zero"):
            watch.validate_event(
                value,
                ".github/vellum-expansion-watch-events/"
                "20260813-exact-boundary-audit-refresh.json",
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
        self.assertIn("added by the same commit", report["errors"][0])

    def claim(self, repo: pathlib.Path, path: str, event_id: str, *,
              value: dict | None = None, text: str | None = None) -> str:
        """One commit that changes a watched path and adds its own event."""
        target = repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text or f"{event_id}\n")
        if value is None:
            value = event("render-assets-and-backends", event_id=event_id)
            value["rationale"] = f"Record the {event_id} render change; no authority moves."
        write_json(repo / watch.EVENT_ROOT / f"{event_id}.json", value)
        return self.commit(repo, f"record {event_id}")

    def test_stacked_disjoint_claims_of_one_family_pass(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        self.claim(repo, "core/render/parent_backend.cpp", "20261127-parent-render-watch")
        head = self.claim(repo, "core/render/child_backend.cpp", "20261127-child-render-watch")
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "pass", report["errors"])
        self.assertEqual(len(report["events"]), 2)

    def test_disjoint_claims_from_merged_branches_pass(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        run(repo, "git", "checkout", "-q", "-b", "parent")
        self.claim(repo, "core/render/parent_backend.cpp", "20261127-parent-render-watch")
        run(repo, "git", "checkout", "-q", "-b", "child", base)
        self.claim(repo, "core/render/child_backend.cpp", "20261127-child-render-watch")
        run(repo, "git", "merge", "-q", "--no-edit", "parent")
        head = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "pass", report["errors"])

    def test_overlapping_claims_of_one_family_fail(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        self.claim(repo, "core/render/backend.cpp", "20261127-parent-render-watch")
        head = self.claim(repo, "core/render/backend.cpp", "20261127-child-render-watch",
                          text="edited again\n")
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "fail")
        self.assertIn("duplicate capability-family claims", report["errors"][0])
        self.assertIn("both cover ['core/render/backend.cpp']", report["errors"][0])

    def test_exact_duplicate_event_fails_even_with_disjoint_paths(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        first = event("render-assets-and-backends", event_id="20261127-render-watch")
        self.claim(repo, "core/render/a.cpp", "20261127-render-watch", value=first)
        copy_of = dict(first, event_id="20261127-render-copy-watch")
        head = self.claim(repo, "core/render/b.cpp", "20261127-render-copy-watch", value=copy_of)
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "fail")
        self.assertIn("record the same change", report["errors"][0])

    def test_shared_rationale_fails_even_with_disjoint_paths(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        first = event("render-assets-and-backends", event_id="20261127-render-watch")
        self.claim(repo, "core/render/a.cpp", "20261127-render-watch", value=first)
        later = dict(first, event_id="20261127-render-later-watch",
                     created_at="2026-11-27T09:00:00Z")
        later["rationale"] = "  " + first["rationale"].replace(" ", "\n ", 1)
        head = self.claim(repo, "core/render/b.cpp", "20261127-render-later-watch", value=later)
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "fail")
        self.assertIn("record the same change", report["errors"][0])

    def test_claim_with_no_path_of_its_own_fails(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        self.claim(repo, "core/render/backend.cpp", "20261127-parent-render-watch")
        value = event("render-assets-and-backends", event_id="20261127-again-render-watch")
        value["rationale"] = "Record the render change a second time; no authority moves."
        write_json(repo / watch.EVENT_ROOT / "20261127-again-render-watch.json", value)
        head = self.commit(repo, "second event, no change of its own")
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "fail")
        self.assertIn("record no path of this family of their own", report["errors"][0])

    def test_event_rewritten_inside_the_range_cannot_share_a_family(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        self.claim(repo, "core/render/parent_backend.cpp", "20261127-parent-render-watch")
        self.claim(repo, "core/render/child_backend.cpp", "20261127-child-render-watch")
        path = repo / watch.EVENT_ROOT / "20261127-child-render-watch.json"
        value = json.loads(path.read_text())
        value["rationale"] += " Edited."
        write_json(path, value)
        head = self.commit(repo, "edit an event inside the range")
        report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "fail")
        self.assertIn("not added by exactly one non-merge commit", report["errors"][0])

    def test_shared_claims_need_base_to_be_an_ancestor(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        seed = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        run(repo, "git", "checkout", "-q", "-b", "side")
        (repo / "side.txt").write_text("side\n")
        side = self.commit(repo, "side")
        run(repo, "git", "checkout", "-q", "-b", "work", seed)
        self.claim(repo, "core/render/parent_backend.cpp", "20261127-parent-render-watch")
        head = self.claim(repo, "core/render/child_backend.cpp", "20261127-child-render-watch")
        report = self.active_verify(repo, side, head)
        self.assertEqual(report["status"], "fail")
        self.assertIn("base is not an ancestor of head", report["errors"][0])

    def test_shared_claims_refused_on_shallow_history(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        self.claim(repo, "core/render/parent_backend.cpp", "20261127-parent-render-watch")
        head = self.claim(repo, "core/render/child_backend.cpp", "20261127-child-render-watch")
        real = watch._git

        def shallow(root, *args, check=True):
            if args == ("rev-parse", "--is-shallow-repository"):
                return subprocess.CompletedProcess(args, 0, b"true\n", b"")
            return real(root, *args, check=check)

        with mock.patch.object(watch, "_git", side_effect=shallow):
            report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "fail")
        self.assertIn("history is shallow", report["errors"][0])

    def test_single_claims_never_read_history(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        head = self.claim(repo, "core/render/backend.cpp", "20261127-render-watch")
        with mock.patch.object(watch, "_commit_paths", side_effect=AssertionError):
            report = self.active_verify(repo, base, head)
        self.assertEqual(report["status"], "pass", report["errors"])

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


    def test_cli_resolves_a_short_head_but_verify_stays_strict(self) -> None:
        temporary, repo = self.git_repo()
        del temporary
        base = run(repo, "git", "rev-parse", "HEAD").stdout.strip()
        (repo / "docs.txt").write_text("unwatched\n", encoding="utf-8")
        head = self.commit(repo, "change docs")
        output = repo / "report.json"
        for base_arg, head_arg in ((base[:10], head[:10]), ("HEAD~1", "HEAD")):
            with mock.patch("sys.stdout"):
                code = watch.main(["--repo", str(repo), "--base", base_arg,
                                   "--head", head_arg, "--output", str(output)])
            report = json.loads(output.read_text())
            self.assertEqual(code, 0, report)
            self.assertEqual(report["status"], "pass", report)
        # verify() is the trusted contract and still demands full SHAs.
        report = watch.verify(repo, base[:10], head[:10])
        self.assertEqual(report["status"], "fail")
        self.assertIn("expected full commit SHA", report["errors"][0])
        # An unresolvable ref is passed through and still refused.
        with mock.patch("sys.stdout"):
            code = watch.main(["--repo", str(repo), "--base", "no-such-ref",
                               "--head", head, "--output", str(output)])
        self.assertEqual(code, 1)
        self.assertIn("expected full commit SHA",
                      json.loads(output.read_text())["errors"][0])


if __name__ == "__main__":
    unittest.main()
