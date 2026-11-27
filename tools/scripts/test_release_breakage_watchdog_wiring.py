#!/usr/bin/env python3
"""Prove both release-breakage watchdogs are connected to their workflows."""

from __future__ import annotations

import shlex
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RELEASE = (ROOT / ".github/workflows/release-cli.yml").read_text(encoding="utf-8")
LIVENESS = (ROOT / ".github/workflows/required-gate-liveness.yml").read_text(encoding="utf-8")
WORKFLOW_LINT = (ROOT / ".github/workflows/workflow-lint.yml").read_text(encoding="utf-8")
# workflow-lint.yml runs on every event and decides scope from this list.
WORKFLOW_LINT_SCOPE = (ROOT / "tools/ci/workflow_lint_paths.txt").read_text(encoding="utf-8").splitlines()


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
        archive = RELEASE.index("tools/scripts/archive_sdk.py")
        self.assertLess(resign, stamp)
        self.assertLess(stamp, archive)

    @staticmethod
    def _invocations(script: str) -> list[list[str]]:
        """Return the argv of each `script` call, joining line continuations."""
        found = []
        lines = RELEASE.splitlines()
        for index, line in enumerate(lines):
            if script not in line:
                continue
            text = line
            while text.rstrip().endswith(("\\", "`")) and index + 1 < len(lines):
                index += 1
                text = text.rstrip()[:-1] + " " + lines[index].strip()
            found.append(shlex.split(text.replace("${{ matrix.platform }}", "PLATFORM").strip()))
        return found

    @staticmethod
    def _options(argv: list[str]) -> dict[str, str]:
        return {argv[i]: argv[i + 1] for i, arg in enumerate(argv[:-1]) if arg.startswith("--")}

    def test_sdk_archives_are_built_and_checked_by_the_normalizing_helper(self) -> None:
        self.assertNotIn("tar czf pulp-sdk-", RELEASE)
        self.assertNotIn("Compress-Archive -Path pulp-sdk", RELEASE)
        builds = self._invocations("tools/scripts/archive_sdk.py")
        checks = self._invocations("tools/scripts/check_archive_mtimes.py")
        self.assertEqual(len(builds), 2)
        self.assertEqual(len(checks), 2)
        for build, check, epoch in zip(builds, checks, ("$SOURCE_DATE_EPOCH", "$sourceEpoch")):
            options = self._options(build)
            self.assertEqual(options["--source"], "pulp-sdk")
            self.assertEqual(options["--output"], "pulp-sdk-PLATFORM.tar.gz")
            self.assertEqual(options["--epoch"], epoch)
            self.assertIn("pulp-sdk-PLATFORM.tar.gz", check)
            self.assertEqual(self._options(check)["--max-mtime"], options["--epoch"])

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
        for path in ("CMakeLists.txt", "**/CMakeLists.txt", "tools/cmake/**"):
            self.assertIn(
                path,
                WORKFLOW_LINT_SCOPE,
                "pull requests, queue batches and main pushes must run release "
                f"matrix parity when {path} changes",
            )

if __name__ == "__main__":
    unittest.main()
