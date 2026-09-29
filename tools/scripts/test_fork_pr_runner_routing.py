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
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
LEGACY_SHARED_LABEL = "pulp-gate-fast"
SELF_HOSTED = (
    '["self-hosted","macOS","ARM64","pulp-build",'
    f'"{LEGACY_SHARED_LABEL}"]'
)
OVERFLOW = (
    '["self-hosted","macOS","ARM64","pulp-build-m5",'
    f'"{LEGACY_SHARED_LABEL}"]'
)
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


def _matrix(head_repo: str | None, *, overflow: str = "local-only",
            event: str = "pull_request",
            dispatch_selector: str = "",
            extra_env: dict[str, str] | None = None) -> list[dict]:
    """Run the real resolver and return its matrix entries."""
    env = dict(os.environ)
    env.update(
        GITHUB_EVENT_NAME=event,
        GITHUB_REPOSITORY="Generous-Corp/pulp",
        LOCAL_MACOS_RUNS_ON_JSON=SELF_HOSTED,
        OVERFLOW_MACOS_RUNS_ON_JSON=overflow,
        WORKFLOW_DISPATCH_MACOS_SELECTOR=dispatch_selector,
        LOCAL_MAC_OVERFLOW_THRESHOLD="0",
        GITHUB_WORKSPACE=str(REPO_ROOT),
    )
    env["PR_HEAD_REPO"] = head_repo or ""
    env.update(extra_env or {})

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
        return json.loads(match.group(1)).get("include", [])


def _macos_runs_on(head_repo: str | None, *, overflow: str = "local-only",
                   event: str = "pull_request",
                   dispatch_selector: str = "") -> Any | None:
    """Run the real resolver and return the macOS leg's runs-on, if any."""
    for entry in _matrix(head_repo, overflow=overflow, event=event,
                         dispatch_selector=dispatch_selector):
        if entry.get("key") == "macos":
            return json.loads(entry["runs_on_json"])
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
        got = _macos_runs_on("someone-else/pulp")
        self.assertIsInstance(got, str)
        self.assertIn("macos", got.lower())

    def test_same_repo_pr_still_uses_the_self_hosted_macs(self):
        # The control. Without it, a resolver that routed *everything* to
        # github-hosted would satisfy every assertion above.
        got = _macos_runs_on("Generous-Corp/pulp")
        self.assertIn("self-hosted", got)
        self.assertIn(PR_HEAD_LABEL, got)
        self.assertNotIn(MERGE_GROUP_LABEL, got)
        self.assertNotIn(LEGACY_SHARED_LABEL, got)

    def test_merge_group_uses_the_higher_priority_event_class(self):
        got = _macos_runs_on(None, event="merge_group")
        self.assertIsInstance(got, list)
        self.assertIn("self-hosted", got)
        self.assertIn(MERGE_GROUP_LABEL, got)
        self.assertNotIn(PR_HEAD_LABEL, got)
        self.assertNotIn(LEGACY_SHARED_LABEL, got)

    def test_default_workflow_dispatch_uses_pr_head_event_class(self):
        got = _macos_runs_on(None, event="workflow_dispatch")
        self.assertIn("self-hosted", got)
        self.assertNotIn(MERGE_GROUP_LABEL, got)
        self.assertIn(PR_HEAD_LABEL, got)
        self.assertNotIn(LEGACY_SHARED_LABEL, got)

    def test_explicit_workflow_dispatch_selector_is_unchanged(self):
        selector = (
            '["self-hosted","macOS","ARM64","pulp-build-vm",'
            '"operator-proof"]'
        )
        got = _macos_runs_on(
            None,
            event="workflow_dispatch",
            dispatch_selector=selector,
        )
        self.assertEqual(got, json.loads(selector))


TOPOLOGY = REPO_ROOT / "tools" / "scripts" / "runner_topology.json"
AUTO_LINUX = (
    '["self-hosted","Linux","X64","pulp-build-linux-x64","pulp-host-macpro",'
    '"pulp-auto-linux-x64"]'
)
_HOSTED_VAR_FALLBACK = re.compile(
    r"""^\$\{\{\s*fromJSON\(vars\.(PULP_[A-Z0-9_]+_RUNS_ON_JSON)\s*\|\|\s*'"[a-z0-9.-]+"'\)\s*\}\}$"""
)
_MATRIX_RUNS_ON = "${{ fromJSON(matrix.runs_on_json) }}"


def _conjuncts(expression: str) -> list[str]:
    """Top-level `&&` terms of a job `if:`, parentheses respected."""
    text = " ".join(str(expression).replace("${{", "").replace("}}", "").split())
    terms, depth, current = [], 0, ""
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if depth == 0 and text.startswith("&&", i):
            terms.append(current.strip())
            current = ""
            i += 2
            continue
        current += ch
        i += 1
    terms.append(current.strip())
    return [t for t in terms if t]


_EVENT_EQ = re.compile(r"^github\.event_name == '([a-z_]+)'$")


def _excludes_push(expression: Any) -> bool:
    """True when some top-level conjunct can never hold on a push event."""
    if expression is None:
        return False
    for term in _conjuncts(expression):
        if term == "github.event_name != 'push'":
            return True
        inner = term[1:-1] if term.startswith("(") and term.endswith(")") else term
        alternatives = [a.strip() for a in inner.split("||")]
        events = [_EVENT_EQ.match(a) for a in alternatives]
        if all(events) and all(m.group(1) != "push" for m in events):
            return True
    return False


def _hosted_lane_variables() -> set[str]:
    topology = json.loads(TOPOLOGY.read_text())
    return {
        lane["variable"]
        for lane in topology["lanes"]
        if lane.get("provisioning") == "github-hosted"
    }


def _hosted_labels() -> set[str]:
    return set(json.loads(TOPOLOGY.read_text())["github_hosted_labels"])


class PushRunsNeverReachSelfHosted(unittest.TestCase):
    """A push to main must finish on GitHub-hosted runners.

    Push runs share main's `build-refs/heads/main` concurrency group with
    cancel-in-progress false, so one job queued for a self-hosted runner holds
    the group and every later push run is cancelled while pending, with zero
    jobs -- which took the Linux/Windows cache-save steps down with it. No
    push-reachable job may route to a self-hosted runner.
    """

    def test_push_matrix_has_no_macos_leg(self):
        self.assertIsNone(_macos_runs_on(None, event="push"))

    def test_push_matrix_is_entirely_hosted(self):
        entries = _matrix(None, event="push",
                          extra_env={"AUTOMATIC_LINUX_RUNNER_SELECTOR_JSON": AUTO_LINUX})
        self.assertTrue(entries, "push produced an empty matrix")
        self.assertEqual({e["key"] for e in entries}, {"linux", "windows"})
        for entry in entries:
            with self.subTest(key=entry["key"]):
                self.assertNotIn("self-hosted", entry["runs_on_json"])

    def test_merge_group_matrix_still_has_the_self_hosted_macos_gate(self):
        """Control: the push guard must not remove the required gate."""
        got = _macos_runs_on(None, event="merge_group")
        self.assertIn("self-hosted", got)

    def test_every_push_reachable_job_routes_to_a_hosted_runner(self):
        try:
            import yaml
        except ImportError:  # pragma: no cover - environment-dependent
            raise unittest.SkipTest("PyYAML not installed")
        jobs = yaml.safe_load(WORKFLOW.read_text())["jobs"]
        hosted_vars = _hosted_lane_variables()
        hosted_labels = _hosted_labels()
        reachable = []
        for name, job in jobs.items():
            if _excludes_push(job.get("if")):
                continue
            reachable.append(name)
            runs_on = job.get("runs-on")
            with self.subTest(job=name, runs_on=runs_on):
                if runs_on == _MATRIX_RUNS_ON:
                    continue  # the resolver tests above pin the push matrix
                if isinstance(runs_on, str) and runs_on in hosted_labels:
                    continue
                match = _HOSTED_VAR_FALLBACK.match(str(runs_on))
                self.assertIsNotNone(
                    match,
                    f"{name} is reachable on push and routes to {runs_on!r}, "
                    "which is not a GitHub-hosted label or hosted lane variable",
                )
                self.assertIn(match.group(1), hosted_vars)
        # Control: the walk saw the push lane's real jobs, including the one
        # that used to take the self-hosted gate selector.
        for expected in ("resolve-provider", "classify", "build", "a2t-protected-event"):
            self.assertIn(expected, reachable)
        self.assertNotIn("local-proof", reachable)
        self.assertNotIn("linux", reachable)

    def test_push_exclusion_parser_controls(self):
        self.assertTrue(_excludes_push("!cancelled() && github.event_name != 'push'"))
        self.assertTrue(_excludes_push(
            "x && (github.event_name == 'pull_request' || github.event_name == 'merge_group')"))
        self.assertFalse(_excludes_push(
            "x && (github.event_name == 'push' || github.event_name == 'merge_group')"))
        self.assertFalse(_excludes_push("!inputs.local_proof && github.event_name == 'push'"))
        self.assertFalse(_excludes_push(None))


if __name__ == "__main__":
    unittest.main(verbosity=2)
