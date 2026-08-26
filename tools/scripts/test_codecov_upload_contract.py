#!/usr/bin/env python3
"""Contract tests for verified, explicit, observable Codecov uploads."""

from __future__ import annotations

import pathlib
import sys
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ACTION = REPO_ROOT / ".github" / "actions" / "upload-codecov-report" / "action.yml"
WORKFLOWS = REPO_ROOT / ".github" / "workflows"
sys.path.insert(0, str(REPO_ROOT / "tools" / "scripts"))

from check_codecov_receipts import (  # noqa: E402
    processed_upload_counts,
    receipt_counts,
    receipt_summary,
)


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

    def test_pull_request_coverage_trigger_cannot_silently_disappear(self) -> None:
        self.assertIn(
            "\n  pull_request:\n",
            self.coverage,
            "coverage.yml contains PR-only jobs and uploads; without the event "
            "trigger Codecov never receives PR coverage",
        )
        self.assertIn(
            "not in protected main's required-check set",
            self.coverage,
            "PR coverage must remain advisory and off the merge critical path",
        )
        self.assertIn(
            "cancel-in-progress: ${{ github.event_name == 'pull_request' }}",
            self.coverage,
            "stale advisory PR coverage must not consume the native fleet",
        )
        self.assertIn("after_n_builds: 1", self.coverage)
        self.assertNotIn("after_n_builds: 4", self.coverage)
        self.assertIn('macos=\'"macos-15"\'', self.coverage)
        self.assertIn('[ "${PR_HEAD_REPO}" != "${THIS_REPO}" ]', self.coverage)

    def test_native_upload_requires_semantically_verified_report(self) -> None:
        self.assertIn("id: native_cobertura", self.coverage)
        self.assertIn(
            "if: always() && (steps.coverage-suite.outputs.budget_hit != 'true' || matrix.os != 'windows')",
            self.coverage,
        )
        self.assertNotIn("steps.coverage-suite.outcome", self.coverage)
        self.assertIn(
            "if: always() && steps.native_cobertura.outcome == 'success' && "
            "github.event.pull_request.head.repo.fork != true",
            self.coverage,
        )
        self.assertIn(
            "uses: ./.github/actions/upload-codecov-report", self.coverage
        )

    def test_required_native_lanes_skip_slow_proofs_and_fail_on_budget_miss(self) -> None:
        self.assertIn("scripts/run_coverage.sh owns the bounded CTest policy", self.coverage)
        self.assertNotIn("PULP_COVERAGE_CTEST_ARGS:", self.coverage)
        self.assertIn(
            "receipt-authoritative native lanes",
            self.coverage,
        )
        self.assertNotIn(
            "marking this leg as a non-fatal skip",
            self.coverage,
        )

    def test_native_budget_terminates_the_full_process_tree(self) -> None:
        self.assertIn("os.setsid()", self.coverage)
        self.assertIn('kill -TERM -- "-${cov}"', self.coverage)
        self.assertIn('$i == "WINPID"', self.coverage)
        self.assertIn('taskkill.exe //PID "${cov_winpid}" //T //F', self.coverage)

    def test_main_watchdog_requires_both_native_upload_receipts(self) -> None:
        self.assertIn("tools/scripts/check_codecov_receipts.py", self.watchdog)
        self.assertIn('"linux=${linux_attempt}" "macos=${macos_attempt}"', self.watchdog)
        self.assertIn('"python-tools=${linux_attempt}"', self.watchdog)
        self.assertIn('-f filter=all', self.watchdog)
        self.assertIn('Coverage report (Linux, Clang)', self.watchdog)
        self.assertIn('Coverage report (macOS, Clang)', self.watchdog)
        self.assertIn('name: Coverage report (${{ matrix.label }}, Clang)', self.coverage)
        self.assertIn('label:"Linux"', self.coverage)
        self.assertIn('label:"macOS"', self.coverage)
        self.assertIn('select(.status=="completed")', self.watchdog)
        self.assertNotIn('select(.conclusion=="success")', self.watchdog)
        self.assertIn("github.event.inputs.max_age_hours || '14'", self.watchdog)
        self.assertIn("--paginate --slurp", self.watchdog)
        self.assertNotIn("--slurp \\\n            --jq", self.watchdog)
        self.assertIn("| jq -c '[.[].workflow_runs[]", self.watchdog)
        self.assertIn('-f created=">=${scan_cutoff}"', self.watchdog)
        self.assertIn(".oldest_created_at", self.watchdog)
        self.assertIn('[[ "${candidate_at}" > "${last_success_at}" ]]', self.watchdog)
        self.assertIn("scan_incomplete=1", self.watchdog)
        self.assertIn("leaving watchdog issue state unchanged", self.watchdog)
        self.assertIn(
            "api.codecov.io/api/v2/github/${owner}/repos/${repo_name}/commits/${head_sha}/uploads/",
            self.watchdog,
        )
        self.assertIn("--codecov-processed-run-id", self.watchdog)
        self.assertIn("--required-flag os-linux os-macos python-tools", self.watchdog)
        self.assertIn('--not-before "${attempt_started_at}"', self.watchdog)
        self.assertIn('codecov_page_url="$(jq -r', self.watchdog)
        self.assertIn('repos/${REPO}/compare/${last_success_sha}...${current_main_sha}', self.watchdog)
        self.assertIn("current-main lag ${coverage_lag}", self.watchdog)
        self.assertIn('if [ "${DRY_RUN}" != "true" ]; then\n              exit 1', self.watchdog)

    def test_receipt_checker_rejects_prefix_wrong_sha_and_expired(self) -> None:
        sha = "abc123"
        artifacts = [
            {"name": f"codecov-upload-linux-{sha}-attempt-2", "expired": False, "created_at": "2026-08-05T10:00:00Z"},
            {"name": f"codecov-upload-macos-{sha}-attempt-2-extra", "expired": False},
            {"name": "codecov-upload-macos-oldsha", "expired": False},
            {"name": f"codecov-upload-python-tools-{sha}-attempt-1", "expired": False},
            {"name": f"codecov-upload-python-tools-{sha}-attempt-2", "expired": True},
        ]
        self.assertEqual(
            receipt_counts(artifacts, sha, {"linux": 2, "macos": 2, "python-tools": 2}),
            {"linux": 1, "macos": 0, "python-tools": 0},
        )

    def test_processed_uploads_require_exact_run_merged_flags(self) -> None:
        repo = "Generous-Corp/pulp"
        run_id = 123
        exact_url = f"https://github.com/{repo}/actions/runs/{run_id}"
        payload = {
            "count": 5,
            "results": [
                {"build_url": exact_url, "state": "merged", "flags": ["os-linux"], "created_at": "2026-08-26T10:01:00Z"},
                {"build_url": exact_url, "state": "merged", "flags": ["os-macos"], "created_at": "2026-08-26T10:02:00Z"},
                {"build_url": exact_url, "state": "merged", "flags": ["python-tools"], "created_at": "2026-08-26T10:03:00Z"},
                {"build_url": exact_url, "state": "merged", "flags": ["pulp-react"], "created_at": "2026-08-26T10:04:00Z"},
                {"build_url": exact_url, "state": "merged", "flags": [], "created_at": "2026-08-26T10:05:00Z"},
            ],
        }
        self.assertEqual(
            processed_upload_counts(
                payload, repo, run_id, ["os-linux", "os-macos", "python-tools"], "2026-08-26T10:00:00Z"
            ),
            {
                "counts": {"os-linux": 1, "os-macos": 1, "python-tools": 1},
                "complete_page": True,
            },
        )

    def test_processed_uploads_reject_wrong_run_pending_and_truncated_page(self) -> None:
        repo = "Generous-Corp/pulp"
        run_id = 123
        exact_url = f"https://github.com/{repo}/actions/runs/{run_id}"
        payload = {
            "count": 4,
            "results": [
                {"build_url": exact_url, "state": "merged", "flags": ["os-linux"], "created_at": "2026-08-26T09:59:00Z"},
                {"build_url": exact_url, "state": "pending", "flags": ["os-macos"], "created_at": "2026-08-26T10:01:00Z"},
                {
                    "build_url": f"https://github.com/{repo}/actions/runs/999",
                    "state": "merged",
                    "flags": ["python-tools"],
                    "created_at": "2026-08-26T10:02:00Z",
                },
            ],
        }
        self.assertEqual(
            processed_upload_counts(
                payload, repo, run_id, ["os-linux", "os-macos", "python-tools"], "2026-08-26T10:00:00Z"
            ),
            {
                "counts": {"os-linux": 0, "os-macos": 0, "python-tools": 0},
                "complete_page": False,
            },
        )

    def test_receipt_checker_accepts_one_exact_current_receipt_per_axis(self) -> None:
        sha = "abc123"
        artifacts = [
            {"name": f"codecov-upload-linux-{sha}-attempt-2", "expired": False, "created_at": "2026-08-05T10:02:00Z"},
            {"name": f"codecov-upload-macos-{sha}-attempt-2", "expired": False, "created_at": "2026-08-05T10:03:00Z"},
            {"name": f"codecov-upload-python-tools-{sha}-attempt-2", "expired": False, "created_at": "2026-08-05T10:01:00Z"},
        ]
        self.assertEqual(
            receipt_counts(artifacts, sha, {"linux": 2, "macos": 2, "python-tools": 2}),
            {"linux": 1, "macos": 1, "python-tools": 1},
        )
        self.assertEqual(
            receipt_summary(artifacts, sha, {"linux": 2, "macos": 2, "python-tools": 2})["oldest_created_at"],
            "2026-08-05T10:01:00Z",
        )

    def test_receipt_summary_rejects_missing_creation_time(self) -> None:
        sha = "abc123"
        artifacts = [
            {"name": f"codecov-upload-linux-{sha}-attempt-2", "expired": False},
            {"name": f"codecov-upload-macos-{sha}-attempt-2", "expired": False, "created_at": "2026-08-05T10:00:00Z"},
            {"name": f"codecov-upload-python-tools-{sha}-attempt-2", "expired": False, "created_at": "2026-08-05T10:00:00Z"},
        ]
        self.assertIsNone(
            receipt_summary(artifacts, sha, {"linux": 2, "macos": 2, "python-tools": 2})["oldest_created_at"]
        )

    def test_receipt_summary_accepts_mixed_latest_axis_attempts(self) -> None:
        sha = "abc123"
        artifacts = [
            {"name": f"codecov-upload-linux-{sha}-attempt-1", "expired": False, "created_at": "2026-08-05T10:00:00Z"},
            {"name": f"codecov-upload-python-tools-{sha}-attempt-1", "expired": False, "created_at": "2026-08-05T10:01:00Z"},
            {"name": f"codecov-upload-macos-{sha}-attempt-2", "expired": False, "created_at": "2026-08-05T10:05:00Z"},
        ]
        summary = receipt_summary(
            artifacts, sha, {"linux": 1, "macos": 2, "python-tools": 1}
        )
        self.assertEqual(summary["counts"], {"linux": 1, "macos": 1, "python-tools": 1})

    def test_native_graph_preserves_example_driven_core_coverage(self) -> None:
        self.assertIn("-DPULP_BUILD_EXAMPLES=ON", self.coverage)
        self.assertIn("matrix.os == 'linux' || matrix.os == 'macos'", self.coverage)
        self.assertIn("-DPULP_BUILD_PYTHON=ON -DPULP_BUILD_EXAMPLES=ON", self.coverage)
        self.assertIn("timeout-minutes: 210", self.coverage)
        self.assertIn("budget=$(( 180 * 60 ))", self.coverage)
        self.assertIn("leaves 30 min for post-suite work", self.coverage)
        self.assertNotIn("coverage_args+=(--test-jobs", self.coverage)
        self.assertNotIn("steps.coverage-suite.outcome }} != \"success\"", self.coverage)
        self.assertNotIn("steps.python_coverage.outcome }} != \"success\"", self.coverage)
        self.assertNotIn(
            "files=\"${files},build-coverage/python/coverage.python.xml\"",
            self.coverage,
        )

    def test_python_upload_is_independent_from_native_report(self) -> None:
        self.assertIn("id: python_coverage", self.coverage)
        self.assertNotIn("steps.python_coverage.outcome", self.coverage)
        self.assertIn("name: Upload Python tools coverage to Codecov", self.coverage)
        self.assertIn(
            "if: always() && steps.python_cobertura.outcome == 'success'",
            self.coverage,
        )
        self.assertIn("flags: python-tools", self.coverage)
        self.assertIn(
            "receipt-name: codecov-upload-python-tools-${{ github.sha }}-attempt-${{ github.run_attempt }}",
            self.coverage,
        )
        self.assertIn(
            "if: always() && steps.native_cobertura.outcome == 'success'",
            self.coverage,
            "a Python verifier failure must not suppress valid native coverage",
        )

    def test_watchdog_has_no_legacy_artifact_fallback(self) -> None:
        self.assertNotIn("legacy_count", self.watchdog)
        self.assertNotIn('startswith("coverage-cobertura-")', self.watchdog)

    def test_success_only_staleness_watchdog_is_retired(self) -> None:
        self.assertFalse(
            (WORKFLOWS / "coverage-staleness-check.yml").exists(),
            "a success-only watchdog can contradict the receipt authority",
        )

    def test_all_coverage_call_sites_use_shared_action(self) -> None:
        expected = {
            "coverage.yml": 4,
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
