#!/usr/bin/env python3
"""Regression tests for advisory macOS runner isolation."""

from __future__ import annotations

import importlib.util
import re
import unittest
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
                for variable in forbidden:
                    self.assertNotIn(variable, text)
                self.assertIsNone(
                    re.search(r"runs-on:.*pulp-(?:build|preamble)", text),
                    "advisory workflow hard-codes a required-pool label",
                )


if __name__ == "__main__":
    unittest.main()
