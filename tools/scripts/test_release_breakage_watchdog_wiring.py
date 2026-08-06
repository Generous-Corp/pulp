#!/usr/bin/env python3
"""Prove both release-breakage watchdogs are connected to their workflows."""

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RELEASE = (ROOT / ".github/workflows/release-cli.yml").read_text(encoding="utf-8")
LIVENESS = (ROOT / ".github/workflows/required-gate-liveness.yml").read_text(encoding="utf-8")
WORKFLOW_LINT = (ROOT / ".github/workflows/workflow-lint.yml").read_text(encoding="utf-8")


class ReleaseContentWiring(unittest.TestCase):
    def test_native_matrix_and_downloaded_draft_both_run_content_verifier(self) -> None:
        self.assertIn("Verify release archive product matrix (Unix)", RELEASE)
        self.assertIn("--native-signatures", RELEASE)
        self.assertIn("--all-platforms --version", RELEASE)
        self.assertIn('--matrix "$publication_matrix"', RELEASE)

    def test_downloaded_draft_uses_trusted_current_provenance_floor(self) -> None:
        self.assertIn(
            'selected["sdk_provenance_floor"] = '
            'authoritative["sdk_provenance_floor"]',
            RELEASE,
        )
        self.assertIn(
            'selected["capability_handoff_floor"] = authoritative[',
            RELEASE,
        )
        self.assertIn(
            '"repos/${REPO}/contents/tools/scripts/'
            'release_product_matrix.json?ref=${DEFAULT_BRANCH}"',
            RELEASE,
        )

    def test_installed_macho_is_resigned_before_handoff_hash_and_sdk_archive(self) -> None:
        resign = RELEASE.index("python3 tools/scripts/resign_macos_release_tree.py sdk-staging")
        stamp = RELEASE.index('python3 "$PULP_SDK_PROVENANCE_HELPER" stamp')
        archive = RELEASE.index("tar czf pulp-sdk-${{ matrix.platform }}.tar.gz")
        self.assertLess(resign, stamp)
        self.assertLess(stamp, archive)

    def test_backfill_uses_tag_matrix_or_pre_contract_fallback(self) -> None:
        self.assertIn('if [ ! -f "$path" ]', RELEASE)
        self.assertIn("tools/scripts/release_product_matrix.json", RELEASE)
        self.assertIn('matrix_ref="$TAG"', RELEASE)
        self.assertIn('matrix_ref=FETCH_HEAD', RELEASE)
        self.assertIn('git show "${matrix_ref}:${matrix}"', RELEASE)


class RequiredGateLivenessWiring(unittest.TestCase):
    def test_liveness_has_unfiltered_push_and_scheduled_backstop(self) -> None:
        self.assertIn("branches: [main]", LIVENESS)
        self.assertIn("schedule:", LIVENESS)
        self.assertNotIn("paths:", LIVENESS)

    def test_liveness_binds_audit_to_target_sha(self) -> None:
        self.assertIn("TARGET_SHA: ${{ inputs.target_sha || github.sha }}", LIVENESS)
        self.assertIn("python3 tools/scripts/required_gate_liveness.py", LIVENESS)

    def test_negative_controls_are_in_blocking_workflow_lint(self) -> None:
        for test in (
            "test_release_artifact_contents.py",
            "test_required_gate_liveness.py",
            "test_release_breakage_watchdog_wiring.py",
            "test_resign_macos_release_tree.py",
        ):
            self.assertIn(test, WORKFLOW_LINT)

    def test_sdk_definition_inputs_trigger_release_matrix_parity(self) -> None:
        for path in (
            "'CMakeLists.txt'",
            "'**/CMakeLists.txt'",
            "'tools/cmake/**'",
        ):
            self.assertEqual(
                WORKFLOW_LINT.count(path),
                2,
                "both pull_request and main push must run release matrix parity "
                f"when {path} changes",
            )


if __name__ == "__main__":
    unittest.main()
