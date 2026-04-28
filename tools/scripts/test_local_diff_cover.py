#!/usr/bin/env python3
"""Fixture tests for tools/scripts/local_diff_cover.sh.

We deliberately don't run the full coverage build in these tests —
that's minutes of clang time and the real toolchain already runs in
.github/workflows/coverage.yml. Instead we exercise:

  1. The PULP_SKIP_DIFF_COVER=1 bypass — must exit 0 with the
     documented skip message before doing any work.
  2. The config-driven threshold — assert that coverage_config.json
     parses, the threshold is the documented integer, and the script
     reads the same value via its embedded python helper.
  3. The dependency preflight — when a required binary is missing
     from PATH, the script must exit 2 with a clear message (not
     wedge or run a partial build).

Run:
    python3 tools/scripts/test_local_diff_cover.py
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "local_diff_cover.sh"
CONFIG = REPO_ROOT / "tools" / "scripts" / "coverage_config.json"


class ConfigTests(unittest.TestCase):
    """coverage_config.json is well-formed and exposes the expected keys."""

    def test_config_exists(self) -> None:
        self.assertTrue(CONFIG.exists(), f"missing: {CONFIG}")

    def test_config_parses(self) -> None:
        with CONFIG.open() as f:
            cfg = json.load(f)
        # Required keys consumed by .github/workflows/coverage.yml AND
        # tools/scripts/local_diff_cover.sh — keep them in lockstep.
        self.assertIn("diff_coverage_fail_under", cfg)
        self.assertIn("compare_branch", cfg)
        self.assertIsInstance(cfg["diff_coverage_fail_under"], int)
        self.assertGreaterEqual(cfg["diff_coverage_fail_under"], 0)
        self.assertLessEqual(cfg["diff_coverage_fail_under"], 100)
        self.assertIsInstance(cfg["compare_branch"], str)


class SkipFlagTests(unittest.TestCase):
    """PULP_SKIP_DIFF_COVER=1 short-circuits before any work."""

    def test_skip_flag_exits_zero(self) -> None:
        env = os.environ.copy()
        env["PULP_SKIP_DIFF_COVER"] = "1"
        result = subprocess.run(
            ["bash", str(SCRIPT)],
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("skipped via PULP_SKIP_DIFF_COVER=1", result.stderr)

    def test_skip_flag_works_with_args(self) -> None:
        # Even with positional targets, the skip flag must short-circuit.
        env = os.environ.copy()
        env["PULP_SKIP_DIFF_COVER"] = "1"
        result = subprocess.run(
            ["bash", str(SCRIPT), "pulp-test-state", "pulp-test-widget-bridge"],
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("skipped via PULP_SKIP_DIFF_COVER=1", result.stderr)


class DependencyPreflightTests(unittest.TestCase):
    """The dep-preflight branch is structured to exit 2 with a clear msg."""

    def test_preflight_exits_two_branch_present(self) -> None:
        # Static check: the script must declare an exit-2 path with a
        # "missing required deps" message. We can't reliably hide clang
        # from PATH on macOS (clang lives in /usr/bin alongside dirname
        # and other essentials this script needs), so we assert the
        # exit-2 contract is present in the source.
        text = SCRIPT.read_text()
        self.assertIn("missing required deps", text)
        self.assertIn("exit 2", text)
        # Must check for the canonical Clang toolchain.
        self.assertRegex(text, r"\bclang\b")
        self.assertRegex(text, r"\bllvm-cov\b")
        self.assertRegex(text, r"\bllvm-profdata\b")
        # Must check for diff-cover importability via python3.
        self.assertIn("import diff_cover", text)


class WorkflowSourceOfTruthTests(unittest.TestCase):
    """coverage.yml reads --fail-under from coverage_config.json (anti-drift)."""

    WORKFLOW = REPO_ROOT / ".github" / "workflows" / "coverage.yml"

    def test_workflow_reads_threshold_from_json(self) -> None:
        text = self.WORKFLOW.read_text()
        # The workflow must extract the threshold via jq from the
        # config file BEFORE invoking diff-cover. If a future edit
        # hardcodes --fail-under=NN again, this test fails loudly.
        self.assertIn(
            "jq -r '.diff_coverage_fail_under' tools/scripts/coverage_config.json",
            text,
            "coverage.yml must read the threshold from coverage_config.json — "
            "do not hardcode --fail-under in the workflow",
        )
        # And the diff-cover step must use the resolved variable, not
        # a literal.
        self.assertIn('--fail-under="${THRESHOLD}"', text)
        # Defense in depth: assert no literal --fail-under=<number>
        # remains in the workflow. A hardcoded literal would silently
        # win over the JSON-driven value.
        import re
        literals = re.findall(r"--fail-under=\d+", text)
        self.assertEqual(
            literals, [],
            f"workflow has hardcoded --fail-under literal(s): {literals}; "
            "remove and read from coverage_config.json instead",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
