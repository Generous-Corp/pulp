#!/usr/bin/env python3
"""Regression tests for advisory macOS runner isolation."""

from __future__ import annotations

import importlib.util
import io
import os
import re
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "scripts" / "resolve_advisory_macos_runner.py"
SPEC = importlib.util.spec_from_file_location("resolve_advisory_macos_runner", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class AdvisoryMacosRunnerPolicyTests(unittest.TestCase):
    def test_defaults_to_hosted_without_local_capacity_theft(self) -> None:
        enabled, selector = MODULE.resolve_selector("", '"macos-15"')
        self.assertTrue(enabled)
        self.assertEqual(selector, '"macos-15"')

    def test_empty_optional_lane_is_disabled(self) -> None:
        self.assertEqual(MODULE.resolve_selector("", ""), (False, '"macos-15"'))

    def test_required_pool_labels_are_rejected(self) -> None:
        for label in ("pulp-build", "pulp-build-vm", "pulp-build-studio", "pulp-preamble"):
            with self.subTest(label=label):
                with self.assertRaisesRegex(ValueError, "merge-gate"):
                    MODULE.resolve_selector(
                        f'["self-hosted","macOS","ARM64","{label}"]',
                        "",
                    )

    def test_distinct_gpu_advisory_lane_is_accepted(self) -> None:
        enabled, selector = MODULE.resolve_selector(
            '["self-hosted","macOS","ARM64","pulp-advisory-gpu"]',
            "",
            require_self_hosted=True,
        )
        self.assertTrue(enabled)
        self.assertIn("pulp-advisory-gpu", selector)

    def test_gpu_advisory_lane_cannot_silently_move_hosted(self) -> None:
        with self.assertRaisesRegex(ValueError, "self-hosted"):
            MODULE.resolve_selector('"macos-15"', "", require_self_hosted=True)

    def test_generic_self_hosted_selector_cannot_match_required_runners(self) -> None:
        selectors = (
            '["self-hosted","macOS","ARM64"]',
            '["macOS","ARM64"]',
            '"macOS"',
            '"ARM64"',
        )
        for selector in selectors:
            for require_self_hosted in (False, True):
                with self.subTest(
                    selector=selector,
                    require_self_hosted=require_self_hosted,
                ):
                    with self.assertRaisesRegex(
                        ValueError, r"(?:self-hosted|pulp-advisory-\*)"
                    ):
                        MODULE.resolve_selector(
                            selector,
                            '"macos-15"',
                            require_self_hosted=require_self_hosted,
                        )

    def test_hosted_configured_selector_does_not_need_advisory_identity(self) -> None:
        for configured in (
            '"macos-14"',
            '"macos-15"',
            '"macos-latest"',
            '"MACOS-26"',
        ):
            with self.subTest(configured=configured):
                enabled, selector = MODULE.resolve_selector(configured, '"macos-14"')
                self.assertTrue(enabled)
                self.assertEqual(selector.casefold(), configured.casefold())

    def test_pull_request_ignores_private_selector(self) -> None:
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            enabled, selector = MODULE.resolve_selector(
                '["self-hosted","macOS","ARM64","pulp-advisory-examples"]',
                '"macos-15"',
                event_name="pull_request",
            )
        self.assertTrue(enabled)
        self.assertEqual(selector, '"macos-15"')
        self.assertIn("workflow_dispatch-only", stderr.getvalue())

    def test_pull_request_disables_private_only_lane_without_fallback(self) -> None:
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            result = MODULE.resolve_selector(
                '["self-hosted","macOS","ARM64","pulp-advisory-gpu"]',
                "",
                require_self_hosted=True,
                event_name="pull_request",
            )
        self.assertEqual(result, (False, '"macos-15"'))
        self.assertIn("workflow_dispatch-only", stderr.getvalue())

    def test_required_self_hosted_lane_cannot_fall_back_to_hosted(self) -> None:
        with self.assertRaisesRegex(ValueError, "self-hosted"):
            MODULE.resolve_selector(
                '["self-hosted","macOS","ARM64","pulp-advisory-gpu"]',
                '"macos-15"',
                require_self_hosted=True,
                event_name="pull_request",
            )

    def test_workflow_dispatch_accepts_private_selector(self) -> None:
        enabled, selector = MODULE.resolve_selector(
            '["self-hosted","macOS","ARM64","pulp-advisory-examples"]',
            '"macos-15"',
            event_name="workflow_dispatch",
        )
        self.assertTrue(enabled)
        self.assertIn("pulp-advisory-examples", selector)

    def test_unknown_hosted_looking_selector_fails_closed(self) -> None:
        for configured in (
            '"macos-999"',
            '"macos-0"',
            '"macos-01"',
            '"macos-15-typo"',
        ):
            with self.subTest(configured=configured):
                with self.assertRaisesRegex(ValueError, r"pulp-advisory-\*"):
                    MODULE.resolve_selector(configured, '"macos-15"')

    def test_advisory_workflows_do_not_reference_required_pool_variables(self) -> None:
        policies = {
            "examples-validation.yml": (
                "PULP_ADVISORY_MACOS_RUNS_ON_JSON",
                ("PULP_LOCAL_MACOS_RUNS_ON_JSON", "PULP_PREAMBLE_RUNS_ON_JSON"),
            ),
            "format-baseline-diff.yml": (
                "PULP_ADVISORY_MACOS_RUNS_ON_JSON",
                ("PULP_LOCAL_MACOS_RUNS_ON_JSON", "PULP_PREAMBLE_RUNS_ON_JSON"),
            ),
            "rmlui-native.yml": (
                "PULP_ADVISORY_MACOS_RUNS_ON_JSON",
                ("PULP_LOCAL_MACOS_RUNS_ON_JSON", "PULP_PREAMBLE_RUNS_ON_JSON"),
            ),
            "web-plugins.yml": (
                "PULP_ADVISORY_GPU_MACOS_RUNS_ON_JSON",
                ("PULP_LOCAL_MACOS_RUNS_ON_JSON", "PULP_PREAMBLE_RUNS_ON_JSON"),
            ),
        }
        for filename, (required, forbidden) in policies.items():
            text = (
                REPO_ROOT / ".github" / "workflows" / filename
            ).read_text(encoding="utf-8")
            with self.subTest(workflow=filename):
                self.assertIn(required, text)
                self.assertIn("resolve_advisory_macos_runner.py", text)
                self.assertIn('--event-name "${{ github.event_name }}"', text)
                for variable in forbidden:
                    self.assertNotIn(variable, text)
                self.assertIsNone(
                    re.search(r"runs-on:.*pulp-(?:build|preamble)", text),
                    "advisory workflow hard-codes a required-pool label",
                )


class RmlUiNativeLaneSuiteTests(unittest.TestCase):
    """run-suite must not go green on a stale report or a failed process."""

    LANE = REPO_ROOT / "tools" / "ci" / "rmlui_native_lane.sh"
    GOOD_XML = (
        '<Catch2TestRun><OverallResultsCases successes="3" failures="0" skips="0"/>'
        "</Catch2TestRun>"
    )

    def run_suite(self, program_body: str, report: Path) -> subprocess.CompletedProcess:
        program = report.parent / "program.sh"
        program.write_text("#!/usr/bin/env bash\n" + program_body, encoding="utf-8")
        program.chmod(0o755)
        return subprocess.run(
            ["bash", str(self.LANE), "run-suite", str(program), str(report)],
            capture_output=True, text=True, env={**os.environ},
        )

    def test_success_then_crash_without_report_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "report.xml"
            ok = self.run_suite(f'cat > "$5" <<\'X\'\n{self.GOOD_XML}\nX\n', report)
            self.assertEqual(ok.returncode, 0, ok.stderr)
            self.assertTrue(report.exists())
            # Loader failure: nonzero exit, no report written; the old one must not count.
            bad = self.run_suite("exit 127\n", report)
            self.assertNotEqual(bad.returncode, 0)
            self.assertFalse(report.exists())

    def test_zero_exit_without_report_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "report.xml"
            report.write_text(self.GOOD_XML, encoding="utf-8")
            result = self.run_suite("exit 0\n", report)
            self.assertNotEqual(result.returncode, 0)

    def test_nonzero_exit_with_good_report_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report = Path(tmp) / "report.xml"
            result = self.run_suite(f'cat > "$5" <<\'X\'\n{self.GOOD_XML}\nX\nexit 1\n', report)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
