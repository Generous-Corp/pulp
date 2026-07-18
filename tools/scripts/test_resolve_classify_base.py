#!/usr/bin/env python3
"""Tests for the classifier's event-aware base resolution + its wiring.

Two layers:

1. The pure function (`resolve_base`). The load-bearing case is `push`:
   resolving to `origin/main` there compares main's tip to itself, so the
   classifier can never tell a docs merge from a core merge.

2. The build.yml wiring. A correct resolver the workflow does not call is
   worth nothing, and the cache-save steps it exists to serve must stay off
   the self-hosted macOS runners that keep these caches on local disk.

Run:  python3 tools/scripts/test_resolve_classify_base.py
"""
from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from resolve_classify_base import DEFAULT_BASE, ZERO_SHA, resolve_base  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
SCRIPT = Path(__file__).with_name("resolve_classify_base.py")

BASE_SHA = "a" * 40
BEFORE_SHA = "b" * 40


class ResolveBaseTests(unittest.TestCase):
    def test_push_uses_event_before_not_origin_main(self) -> None:
        # The whole point. On a push to main, HEAD *is* origin/main, so
        # `origin/main...HEAD` is an empty self-comparison.
        self.assertEqual(
            resolve_base("push", pr_base_sha="", push_before_sha=BEFORE_SHA),
            BEFORE_SHA,
        )

    def test_push_ignores_a_pull_request_base_sha(self) -> None:
        # A stale PR base in the environment must not leak into a push run.
        self.assertEqual(
            resolve_base("push", pr_base_sha=BASE_SHA, push_before_sha=BEFORE_SHA),
            BEFORE_SHA,
        )

    def test_pull_request_uses_the_pinned_base_sha(self) -> None:
        self.assertEqual(
            resolve_base("pull_request", pr_base_sha=BASE_SHA, push_before_sha=""),
            BASE_SHA,
        )

    def test_pull_request_target_uses_the_pinned_base_sha(self) -> None:
        self.assertEqual(
            resolve_base("pull_request_target", pr_base_sha=BASE_SHA),
            BASE_SHA,
        )

    def test_pull_request_ignores_a_push_before_sha(self) -> None:
        self.assertEqual(
            resolve_base("pull_request", pr_base_sha=BASE_SHA, push_before_sha=BEFORE_SHA),
            BASE_SHA,
        )

    def test_merge_group_defaults_to_origin_main(self) -> None:
        # A merge group's tree is genuinely meant to be compared against main.
        self.assertEqual(resolve_base("merge_group"), DEFAULT_BASE)

    def test_workflow_dispatch_defaults_to_origin_main(self) -> None:
        self.assertEqual(resolve_base("workflow_dispatch"), DEFAULT_BASE)

    def test_unknown_and_empty_events_default_to_origin_main(self) -> None:
        self.assertEqual(resolve_base("schedule"), DEFAULT_BASE)
        self.assertEqual(resolve_base(""), DEFAULT_BASE)

    def test_push_creating_a_ref_falls_back_to_origin_main(self) -> None:
        # `github.event.before` is the all-zero sha when a push creates a ref.
        # Fail-closed: fall back rather than hand git a bogus ref.
        self.assertEqual(
            resolve_base("push", push_before_sha=ZERO_SHA), DEFAULT_BASE
        )

    def test_malformed_or_missing_shas_fall_back_to_origin_main(self) -> None:
        for bad in ("", "   ", "not-a-sha", "abc123", "z" * 40, "a" * 39, None):
            with self.subTest(sha=bad):
                self.assertEqual(
                    resolve_base("push", push_before_sha=bad), DEFAULT_BASE
                )
                self.assertEqual(
                    resolve_base("pull_request", pr_base_sha=bad), DEFAULT_BASE
                )

    def test_shas_are_stripped_and_case_insensitive(self) -> None:
        self.assertEqual(
            resolve_base("push", push_before_sha=f"  {BEFORE_SHA.upper()}  "),
            BEFORE_SHA.upper(),
        )


class CliTests(unittest.TestCase):
    def _run(self, **env: str) -> str:
        environ = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
        environ.update(env)
        proc = subprocess.run(
            [sys.executable, str(SCRIPT)],
            capture_output=True,
            text=True,
            check=False,
            env=environ,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return proc.stdout.strip()

    def test_cli_reads_push_context_from_env(self) -> None:
        self.assertEqual(
            self._run(GITHUB_EVENT_NAME="push", PUSH_BEFORE_SHA=BEFORE_SHA),
            BEFORE_SHA,
        )

    def test_cli_reads_pull_request_context_from_env(self) -> None:
        self.assertEqual(
            self._run(GITHUB_EVENT_NAME="pull_request", PR_BASE_SHA=BASE_SHA),
            BASE_SHA,
        )

    def test_cli_defaults_with_no_context(self) -> None:
        self.assertEqual(self._run(GITHUB_EVENT_NAME=""), DEFAULT_BASE)


class WorkflowWiringTests(unittest.TestCase):
    """The resolver only pays off if build.yml actually routes through it."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = BUILD_WORKFLOW.read_text(encoding="utf-8")

    def test_classify_calls_the_resolver_with_both_event_shas(self) -> None:
        self.assertIn("tools/scripts/resolve_classify_base.py", self.text)
        self.assertIn(
            "PR_BASE_SHA: ${{ github.event.pull_request.base.sha }}", self.text
        )
        self.assertIn("PUSH_BEFORE_SHA: ${{ github.event.before }}", self.text)

    def test_classify_no_longer_hardcodes_origin_main_as_the_base(self) -> None:
        # The dead inline expression this replaces.
        self.assertNotIn(
            "github.event.pull_request.base.sha || 'origin/main'", self.text
        )

    def test_workflow_triggers_on_push_to_main(self) -> None:
        # Without this trigger the cache-save steps below are unreachable:
        # the workflow simply never runs on a main-branch push.
        self.assertRegex(self.text, r"(?m)^  push:\n    branches: \[main\]$")

    def test_push_runs_are_not_cancelled_by_concurrency(self) -> None:
        # Cancelling a superseded main run throws away the cache it exists to
        # publish. PR runs must still cancel.
        self.assertIn(
            "cancel-in-progress: ${{ github.event_name != 'push' }}", self.text
        )


class CacheSaveScopeTests(unittest.TestCase):
    """The cache-save steps must fire on main pushes, and only off macOS.

    The local self-hosted Macs keep ccache + FetchContent on disk between
    jobs. Round-tripping those multi-GB caches through GitHub's cloud cache
    would slow down the one required gate in the repo to no benefit.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = BUILD_WORKFLOW.read_text(encoding="utf-8")

    def _save_conditions(self) -> list[str]:
        conditions = [
            line.strip()
            for line in self.text.splitlines()
            if line.strip().startswith("if:") and "cache-primary-key" not in line
        ]
        return [c for c in conditions if "outputs.cache-hit != 'true'" in c]

    def test_both_save_steps_exist(self) -> None:
        self.assertIn("- name: Save FetchContent sources", self.text)
        self.assertIn("- name: Save ccache", self.text)
        self.assertEqual(len(self._save_conditions()), 2)

    def test_saves_fire_on_push_to_main_only(self) -> None:
        for condition in self._save_conditions():
            with self.subTest(condition=condition):
                self.assertIn("github.event_name == 'push'", condition)
                self.assertIn("github.ref == 'refs/heads/main'", condition)
                # The old gate. `!= 'pull_request'` is true on merge_group and
                # workflow_dispatch too, which is not what we mean.
                self.assertNotIn("github.event_name != 'pull_request'", condition)

    def test_saves_are_scoped_to_github_hosted_linux_and_windows(self) -> None:
        for condition in self._save_conditions():
            with self.subTest(condition=condition):
                self.assertIn("runner.environment == 'github-hosted'", condition)
                self.assertIn("runner.os != 'macOS'", condition)
                # The restore-side disjunction would re-admit self-hosted
                # non-macOS runners to the save path.
                self.assertNotIn("runner.os != 'macOS' ||", condition)

    def test_macos_leg_is_dropped_from_the_push_matrix(self) -> None:
        # A push run exists only to publish Linux/Windows caches. Scheduling a
        # macOS leg would put the required gate's self-hosted runners under
        # load for a build whose cache is never saved.
        self.assertIn('if EVENT_NAME not in PUSH_ONLY_CACHE_EVENTS:', self.text)
        self.assertIn('PUSH_ONLY_CACHE_EVENTS = ("push",)', self.text)

    def test_reporting_aliases_and_extra_gates_skip_push_runs(self) -> None:
        # The `macos` alias polls for a macOS job for up to 60 minutes before
        # failing; with the leg dropped it would spin the whole time.
        self.assertEqual(
            self.text.count("if: always() && github.event_name != 'push'"), 3
        )
        # windows-msvc-release-gate, windows-midi2-gate, windows-ble-gate.
        self.assertEqual(
            self.text.count(
                "if: needs.classify.outputs.native_build_required == 'true'"
                " && github.event_name != 'push'"
            ),
            3,
        )
        # Every windows-latest job is accounted for above — a new one must not
        # silently start running on cache-warming pushes.
        self.assertEqual(self.text.count("runs-on: windows-latest"), 3)


class PushDiffIntegrationTests(unittest.TestCase):
    """End-to-end: the resolved base must produce a real diff on a push.

    Builds a throwaway repo where a core change lands on main, then drives the
    resolver + classifier exactly as the workflow does. Pins the actual claim —
    `origin/main` on a push is a self-comparison — rather than trusting it.
    """

    def _git(self, *args: str) -> str:
        proc = subprocess.run(
            ["git", *args], cwd=self.work, capture_output=True, text=True, check=True
        )
        return proc.stdout.strip()

    def setUp(self) -> None:
        import tempfile

        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        subprocess.run(
            ["git", "init", "-q", "--bare", "remote.git"],
            cwd=root, check=True, capture_output=True,
        )
        subprocess.run(
            ["git", "clone", "-q", "remote.git", "work"],
            cwd=root, check=True, capture_output=True,
        )
        self.work = root / "work"
        self._git("config", "user.email", "ci@example.invalid")
        self._git("config", "user.name", "ci")
        self._git("symbolic-ref", "HEAD", "refs/heads/main")

        (self.work / "docs").mkdir()
        (self.work / "docs" / "README.md").write_text("seed\n")
        self._git("add", "docs/README.md")
        self._git("commit", "-qm", "seed")
        self._git("push", "-q", "origin", "main")
        self.before = self._git("rev-parse", "HEAD")

        (self.work / "core").mkdir()
        (self.work / "core" / "x.cpp").write_text("int main() { return 0; }\n")
        self._git("add", "core/x.cpp")
        self._git("commit", "-qm", "core change")
        self._git("push", "-q", "origin", "main")
        self._git("fetch", "-q", "origin", "main")

    def _classify(self, base: str) -> dict:
        import json

        proc = subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "tools" / "scripts" / "classify_changes.py"),
                "--mode=diff",
                "--base",
                base,
                "--json",
            ],
            cwd=self.work, capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return json.loads(proc.stdout)

    def test_push_head_and_origin_main_are_the_same_commit(self) -> None:
        # The precondition that makes `origin/main` useless as a push base.
        self.assertEqual(
            self._git("rev-parse", "HEAD"), self._git("rev-parse", "origin/main")
        )

    def test_origin_main_base_sees_no_files_on_a_push(self) -> None:
        self.assertEqual(self._classify("origin/main")["changed_file_count"], 0)

    def test_resolved_push_base_sees_the_real_change(self) -> None:
        base = resolve_base("push", push_before_sha=self.before)
        self.assertEqual(base, self.before)
        result = self._classify(base)
        self.assertEqual(result["changed_file_count"], 1)
        self.assertIn("core/x.cpp", result["reason"])
        self.assertTrue(result["native_build_required"])

    def test_docs_only_push_is_classified_skip_safe(self) -> None:
        # The payoff: with a correct base the classifier can finally tell a
        # docs merge from a core merge. With `origin/main` both look identical
        # (empty -> fail-closed -> build), so the skip never fires on main.
        before = self._git("rev-parse", "HEAD")
        (self.work / "docs" / "README.md").write_text("docs only\n")
        self._git("commit", "-qam", "docs change")
        self._git("push", "-q", "origin", "main")
        self._git("fetch", "-q", "origin", "main")

        result = self._classify(resolve_base("push", push_before_sha=before))
        self.assertEqual(result["changed_file_count"], 1)
        self.assertFalse(result["native_build_required"])
        # Same push, old base: indistinguishable from a core merge.
        self.assertTrue(self._classify("origin/main")["native_build_required"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
