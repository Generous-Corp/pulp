#!/usr/bin/env python3
"""resolve_runs_on.py never hands a fork pull request, deleted or not, a variable-selected runner."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().with_name("resolve_runs_on.py")
REPO = "Generous-Corp/pulp"
SELF_HOSTED = json.dumps(["self-hosted", "macOS", "ARM64", "pulp-sanitizer"])
DEFAULT_ARGS = ["--target-name", "ASan (macOS)", "--mode", "default", "--explicit-env", "EXPLICIT",
                "--override-env", "OVERRIDE", "--default-label", "macos-15"]


def resolve(event: str, head: str | None, args: list[str] = DEFAULT_ARGS, **extra: str):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("GITHUB_", "PR_HEAD"))}
    env.update({"GITHUB_EVENT_NAME": event, "GITHUB_REPOSITORY": REPO, "EXPLICIT": "", "OVERRIDE": SELF_HOSTED,
                **extra})
    if head is not None:
        env["PR_HEAD_REPO"] = head
    proc = subprocess.run([sys.executable, str(SCRIPT), *args], env=env, capture_output=True, text=True,
                          encoding="utf-8")
    return proc.returncode, proc.stdout, proc.stderr


class ForkPullRequests(unittest.TestCase):
    def test_a_fork_or_deleted_fork_pull_request_gets_the_hosted_fallback(self):
        for event in ("pull_request", "pull_request_target"):
            for head in ("someone/pulp", "", None):
                with self.subTest(event=event, head=head):
                    rc, out, err = resolve(event, head)
                    self.assertEqual(rc, 0, err)
                    self.assertEqual(json.loads(out), "macos-15")
                    self.assertIn("fork pull request", err)

    def test_a_dispatch_input_does_not_help_a_fork(self):
        rc, out, _ = resolve("pull_request", "", EXPLICIT=SELF_HOSTED)
        self.assertEqual((rc, json.loads(out)), (0, "macos-15"))

    def test_the_control_same_repo_and_non_pr_events_still_read_the_variable(self):
        """Without this, a resolver that sent everything to hosted would pass the tests above."""
        for event, head in (("pull_request", REPO), ("push", None), ("merge_group", None),
                            ("schedule", None), ("workflow_dispatch", None)):
            with self.subTest(event=event):
                rc, out, err = resolve(event, head)
                self.assertEqual(rc, 0, err)
                self.assertEqual(json.loads(out), json.loads(SELF_HOSTED))

    def test_provider_mode_falls_back_to_the_hosted_label(self):
        args = ["--target-name", "Linux", "--mode", "provider", "--github-hosted-label", "ubuntu-latest",
                "--explicit-env", "OVERRIDE"]
        rc, out, _ = resolve("pull_request", None, args)
        self.assertEqual((rc, json.loads(out)), (0, "ubuntu-latest"))

    def test_optional_namespace_mode_gives_a_fork_nothing(self):
        args = ["--target-name", "macOS", "--mode", "optional-namespace", "--explicit-env", "OVERRIDE"]
        rc, out, _ = resolve("pull_request", "", args)
        self.assertEqual((rc, out), (0, ""))

    def test_a_fallback_that_is_not_hosted_is_refused_for_a_fork(self):
        args = DEFAULT_ARGS[:-1] + ["pulp-build"]
        rc, out, err = resolve("pull_request", "someone/pulp", args)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")
        self.assertIn("GitHub-hosted", err)


if __name__ == "__main__":
    unittest.main()
