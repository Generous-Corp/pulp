#!/usr/bin/env python3
"""Tests for install_shipyard_token_check.py.

Run:
    python3 tools/scripts/test_install_shipyard_token_check.py
"""

from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

import yaml

SCRIPTS = Path(__file__).resolve().parent
REPO_ROOT = SCRIPTS.parent.parent
sys.path.insert(0, str(SCRIPTS))

import install_shipyard_token_check as check  # noqa: E402


def _doc(step_env=None, job_env=None, workflow_env=None, run="./tools/install-shipyard.sh"):
    step = {"name": "Install pinned Shipyard", "run": run}
    if step_env is not None:
        step["env"] = step_env
    job = {"runs-on": "ubuntu-latest", "steps": [step]}
    if job_env is not None:
        job["env"] = job_env
    doc = {"jobs": {"build": job}}
    if workflow_env is not None:
        doc["env"] = workflow_env
    return doc


class TokenCheckTests(unittest.TestCase):
    def test_tokenless_step_is_flagged(self) -> None:
        violations = check.check_doc(_doc(), "wf.yml")
        self.assertEqual(len(violations), 1)
        self.assertIn("GITHUB_TOKEN: ${{ github.token }}", violations[0])

    def test_step_job_and_workflow_env_each_satisfy(self) -> None:
        token = {"GITHUB_TOKEN": "${{ github.token }}"}
        self.assertEqual(check.check_doc(_doc(step_env=token), "wf"), [])
        self.assertEqual(check.check_doc(_doc(job_env=token), "wf"), [])
        self.assertEqual(check.check_doc(_doc(workflow_env=token), "wf"), [])

    def test_shipyard_specific_token_satisfies(self) -> None:
        doc = _doc(step_env={"SHIPYARD_GITHUB_TOKEN": "${{ secrets.X }}"})
        self.assertEqual(check.check_doc(doc, "wf"), [])

    def test_empty_or_unrelated_env_does_not_satisfy(self) -> None:
        self.assertEqual(len(check.check_doc(_doc(step_env={"GITHUB_TOKEN": ""}), "wf")), 1)
        self.assertEqual(len(check.check_doc(_doc(step_env={"GH_TOKEN": "x"}), "wf")), 1)

    def test_steps_that_do_not_install_are_ignored(self) -> None:
        self.assertEqual(check.check_doc(_doc(run="echo hi"), "wf"), [])

    def test_repo_workflows_pass_and_the_scan_saw_installers(self) -> None:
        # Control: the scan must actually reach at least one installing step,
        # or a green result would only prove it looked at nothing.
        workflows = sorted((REPO_ROOT / ".github" / "workflows").glob("*.y*ml"))
        installing = sum(
            len(list(check.installing_steps(yaml.safe_load(p.read_text(encoding="utf-8")))))
            for p in workflows
        )
        self.assertGreaterEqual(installing, 2)
        result = subprocess.run(
            [sys.executable, str(SCRIPTS / "install_shipyard_token_check.py")],
            cwd=REPO_ROOT, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, msg=result.stderr)


if __name__ == "__main__":
    unittest.main()
