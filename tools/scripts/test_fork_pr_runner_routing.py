#!/usr/bin/env python3
"""A fork's code must never be routed onto the self-hosted Macs.

Those hosts carry the Developer ID signing keychain and the notary key, and
``PULP_LOCAL_MACOS_RUNS_ON_JSON`` is a repo *variable* — variables, unlike
secrets, do resolve for fork pull requests. So without a guard in the resolver,
a single "Approve and run" click on a fork PR executes contributor code on the
credentialed machines.

This extracts the runner resolver that ``build.yml`` actually embeds and runs it,
rather than re-implementing the decision here. A test that models the logic it is
guarding would keep passing after the workflow stopped doing this.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
SELF_HOSTED = '["self-hosted","macOS","ARM64","pulp-build"]'
OVERFLOW = '["self-hosted","macOS","ARM64","pulp-build-m5"]'
MERGE_GROUP_LABEL = "pulp-build-merge-group"
PR_HEAD_LABEL = "pulp-build-pr-head"


def _resolver_source() -> str:
    """The Python block from build.yml's runner-resolution step."""
    try:
        import yaml
    except ImportError:  # pragma: no cover - environment-dependent
        raise unittest.SkipTest("PyYAML not installed")

    doc = yaml.safe_load(WORKFLOW.read_text())
    steps = doc["jobs"]["resolve-provider"]["steps"]
    step = next(
        (s for s in steps if "PR_HEAD_REPO" in str(s.get("env", ""))),
        None,
    )
    if step is None:
        raise AssertionError(
            "no resolve-provider step exposes PR_HEAD_REPO — the fork guard is "
            "gone, or its plumbing was renamed"
        )
    match = re.search(r"python3 - <<'PY'\n(.*?)\nPY\n", step["run"], re.S)
    if match is None:
        raise AssertionError("could not extract the embedded Python resolver")
    return match.group(1)


def _macos_runs_on(head_repo: str | None, *, overflow: str = "local-only",
                   event: str = "pull_request") -> str | None:
    """Run the real resolver and return the macOS leg's runs-on, if any."""
    env = dict(os.environ)
    env.update(
        GITHUB_EVENT_NAME=event,
        GITHUB_REPOSITORY="Generous-Corp/pulp",
        LOCAL_MACOS_RUNS_ON_JSON=SELF_HOSTED,
        OVERFLOW_MACOS_RUNS_ON_JSON=overflow,
        LOCAL_MAC_OVERFLOW_THRESHOLD="0",
        GITHUB_WORKSPACE=str(REPO_ROOT),
    )
    env["PR_HEAD_REPO"] = head_repo or ""

    with tempfile.TemporaryDirectory() as tmp:
        script = Path(tmp) / "resolver.py"
        script.write_text(_resolver_source())
        out = Path(tmp) / "github_output"
        out.write_text("")
        env["GITHUB_OUTPUT"] = str(out)
        proc = subprocess.run(
            [sys.executable, str(script)], env=env, cwd=tmp,
            capture_output=True, text=True,
        )
        if proc.returncode != 0:
            raise AssertionError(f"resolver failed: {proc.stderr[-2000:]}")
        match = re.search(r"matrix_json=(.*)", out.read_text())
        if match is None:
            raise AssertionError("resolver emitted no matrix_json")
        for entry in json.loads(match.group(1)).get("include", []):
            if entry.get("key") == "macos":
                return json.dumps(entry["runs_on_json"])
    return None


class ForkPullRequestRunnerRouting(unittest.TestCase):
    def test_fork_pr_never_reaches_the_self_hosted_macs(self):
        got = _macos_runs_on("someone-else/pulp")
        self.assertIsNotNone(got, "fork PR lost its macOS leg entirely")
        self.assertNotIn("self-hosted", got)
        self.assertNotIn("pulp-build", got)

    def test_fork_pr_cannot_reach_the_overflow_mac_either(self):
        got = _macos_runs_on("someone-else/pulp", overflow=OVERFLOW)
        self.assertNotIn("self-hosted", got)
        self.assertNotIn("pulp-build-m5", got)

    def test_fork_pr_still_gets_a_macos_leg(self):
        # Blanking the selector rather than skipping the job is deliberate: the
        # contributor gets a real signal on a clean throwaway runner instead of
        # a required check that never posts.
        self.assertIn("macos", _macos_runs_on("someone-else/pulp").lower())

    def test_same_repo_pr_still_uses_the_self_hosted_macs(self):
        # The control. Without it, a resolver that routed *everything* to
        # github-hosted would satisfy every assertion above.
        got = _macos_runs_on("Generous-Corp/pulp")
        self.assertIn("self-hosted", got)
        self.assertIn(PR_HEAD_LABEL, got)
        self.assertNotIn(MERGE_GROUP_LABEL, got)

    def test_merge_group_uses_the_higher_priority_event_class(self):
        got = _macos_runs_on(None, event="merge_group")
        self.assertIn("self-hosted", got)
        self.assertIn(MERGE_GROUP_LABEL, got)
        self.assertNotIn(PR_HEAD_LABEL, got)

    def test_non_pull_request_events_are_untouched(self):
        got = _macos_runs_on(None, event="workflow_dispatch")
        self.assertIn("self-hosted", got)
        self.assertNotIn(MERGE_GROUP_LABEL, got)
        self.assertNotIn(PR_HEAD_LABEL, got)


if __name__ == "__main__":
    unittest.main(verbosity=2)
