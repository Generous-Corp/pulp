#!/usr/bin/env python3
"""Contract tests for verified, explicit, observable Codecov uploads."""

from __future__ import annotations

import pathlib
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ACTION = REPO_ROOT / ".github" / "actions" / "upload-codecov-report" / "action.yml"
WORKFLOWS = REPO_ROOT / ".github" / "workflows"


class SharedUploadActionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.action = ACTION.read_text(encoding="utf-8")

    def test_action_owns_the_only_direct_codecov_invocation(self) -> None:
        direct = []
        for path in (REPO_ROOT / ".github").rglob("*.yml"):
            if "uses: codecov/codecov-action@" in path.read_text(encoding="utf-8"):
                direct.append(path.relative_to(REPO_ROOT).as_posix())
        self.assertEqual(
            direct, [".github/actions/upload-codecov-report/action.yml"]
        )

    def test_action_requires_explicit_files_and_surfaces_transport_failure(self) -> None:
        self.assertIn("Validate explicit report files", self.action)
        self.assertIn("disable_search: true", self.action)
        self.assertIn("fail_ci_if_error: true", self.action)
        self.assertIn("continue-on-error: true", self.action)
        self.assertIn("steps.upload.outcome == 'success'", self.action)
        self.assertIn("actions/upload-artifact@v6", self.action)


class CoverageWorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.coverage = (WORKFLOWS / "coverage.yml").read_text(encoding="utf-8")
        self.watchdog = (WORKFLOWS / "coverage-upload-watchdog.yml").read_text(
            encoding="utf-8"
        )

    def test_native_upload_requires_semantically_verified_report(self) -> None:
        self.assertIn("id: native_cobertura", self.coverage)
        self.assertIn(
            "if: steps.native_cobertura.outcome == 'success' && "
            "github.event.pull_request.head.repo.fork != true",
            self.coverage,
        )
        self.assertIn(
            "uses: ./.github/actions/upload-codecov-report", self.coverage
        )

    def test_main_watchdog_requires_both_native_upload_receipts(self) -> None:
        self.assertIn('startswith("codecov-upload-linux-")', self.watchdog)
        self.assertIn('startswith("codecov-upload-macos-")', self.watchdog)
        self.assertIn(
            '[ "${has_linux}" -gt 0 ] && [ "${has_macos}" -gt 0 ]',
            self.watchdog,
        )

    def test_all_coverage_call_sites_use_shared_action(self) -> None:
        expected = {
            "coverage.yml": 3,
            "pulp-react-build.yml": 1,
        }
        for name, count in expected.items():
            text = (WORKFLOWS / name).read_text(encoding="utf-8")
            self.assertEqual(
                text.count("uses: ./.github/actions/upload-codecov-report"),
                count,
                name,
            )


if __name__ == "__main__":
    unittest.main()
