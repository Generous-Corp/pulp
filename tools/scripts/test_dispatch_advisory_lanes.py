#!/usr/bin/env python3
"""Real entry-point contract tests for dispatch_advisory_lanes.sh."""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/scripts/dispatch_advisory_lanes.sh"
CMD_PR = ROOT / "tools/cli/cmd_pr.cpp"


class AdvisoryDispatchTests(unittest.TestCase):
    def test_dispatches_every_advisory_workflow_at_the_supplied_head(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            log = Path(td) / "gh.log"
            fake = Path(td) / "gh"
            fake.write_text("#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$GH_LOG\"\n", encoding="utf-8")
            fake.chmod(0o755)
            env = {**os.environ, "GH_BIN": str(fake), "GH_LOG": str(log)}
            result = subprocess.run(
                [str(SCRIPT), "abc123"], env=env, text=True, encoding="utf-8", capture_output=True
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            calls = log.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(calls), 6)
            for workflow in (
                "coverage.yml", "sanitizers.yml", "intel-portability.yml",
                "scene3d-advisory.yml", "examples-validation.yml",
                "control-shipping-native.yml",
            ):
                self.assertIn(
                    f"workflow run {workflow} --repo Generous-Corp/pulp --ref abc123",
                    calls,
                )

    def test_invalid_head_is_rejected_without_dispatch(self) -> None:
        result = subprocess.run(
            [str(SCRIPT), "bad ref"], text=True, encoding="utf-8", capture_output=True
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid head ref", result.stderr)

    def test_pulp_pr_hook_skips_unarmed_runs_and_invokes_after_ship(self) -> None:
        source = CMD_PR.read_text(encoding="utf-8")
        self.assertIn("dispatch_advisory_lanes.sh", source)
        self.assertIn("--no-arm", source)
        self.assertIn("step_shipyard_ship", source)


if __name__ == "__main__":
    unittest.main()
