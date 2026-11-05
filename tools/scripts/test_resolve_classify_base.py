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
import re
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from resolve_classify_base import (  # noqa: E402
    DEFAULT_BASE,
    MAX_QUEUE_WALK,
    ZERO_SHA,
    merge_group_main_base,
    resolve_base,
)

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
# Spelled as whole literals so test/ctest_script_inputs.json records them as
# this test's inputs: a change to either file then selects this test.
BUILD_WORKFLOW = REPO_ROOT / ".github/workflows/build.yml"
SHIPYARD_PROFILE = REPO_ROOT / ".shipyard/ci-profiles/normal-local-fast.toml"
SCRIPT = Path(__file__).with_name("resolve_classify_base.py")

BASE_SHA = "a" * 40
BEFORE_SHA = "b" * 40

# A top-level job header under `jobs:` (two-space indent, then `name:`).
JOB_HEADER = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$", re.MULTILINE)


def job_block(text: str, name: str) -> str:
    """One job's YAML, from its header to the next top-level job.

    Bounded by the next job rather than by a named neighbour, so a job inserted
    after it does not leak into the slice.
    """
    headers = list(JOB_HEADER.finditer(text))
    for index, header in enumerate(headers):
        if header.group(1) == name:
            end = headers[index + 1].start() if index + 1 < len(headers) else len(text)
            return text[header.start() : end]
    raise AssertionError(f"job {name!r} not found in {BUILD_WORKFLOW}")


def job_condition(text: str, name: str) -> str:
    """A job's `if:` condition, folded onto one line."""
    block = job_block(text, name)
    match = re.search(r"^    if: (?:>-\s*\n)?(.*?)(?=^    [a-z-]+:)", block, re.MULTILINE | re.DOTALL)
    if not match:
        raise AssertionError(f"job {name!r} has no if: condition")
    return " ".join(match.group(1).split())


def jobs_on(text: str, runs_on: str) -> list[str]:
    return [
        header.group(1)
        for header in JOB_HEADER.finditer(text)
        if f"\n    runs-on: {runs_on}\n" in job_block(text, header.group(1))
    ]


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

    def test_merge_group_starts_from_the_pinned_base_sha(self) -> None:
        # The pure resolver returns the event's base_sha; resolve_from_env
        # then walks it down to main (MergeGroupMainBaseTests).
        self.assertEqual(
            resolve_base("merge_group", merge_group_base_sha=BASE_SHA), BASE_SHA
        )

    def test_merge_group_without_a_valid_base_fails_back(self) -> None:
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

    def test_cli_merge_group_without_a_base_ref_fails_closed(self) -> None:
        # base_sha alone cannot say whether it is main or a speculative group
        # ahead in the queue, so without main's ref the resolver must not
        # vouch for it. The fallback makes every caller build natively.
        self.assertEqual(
            self._run(
                GITHUB_EVENT_NAME="merge_group",
                MERGE_GROUP_BASE_SHA=BASE_SHA,
            ),
            DEFAULT_BASE,
        )

    def test_cli_defaults_with_no_context(self) -> None:
        self.assertEqual(self._run(GITHUB_EVENT_NAME=""), DEFAULT_BASE)


class WorkflowWiringTests(unittest.TestCase):
    """The resolver only pays off if build.yml actually routes through it."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = BUILD_WORKFLOW.read_text(encoding="utf-8")

    def test_classify_calls_the_resolver_with_event_shas(self) -> None:
        self.assertIn("tools/scripts/resolve_classify_base.py", self.text)
        self.assertIn(
            "PR_BASE_SHA: ${{ github.event.pull_request.base.sha }}", self.text
        )
        self.assertIn("PUSH_BEFORE_SHA: ${{ github.event.before }}", self.text)
        self.assertIn(
            "MERGE_GROUP_BASE_SHA: ${{ github.event.merge_group.base_sha }}",
            self.text,
        )

    def test_merge_group_base_ref_reaches_every_resolver_call(self) -> None:
        # Without base_ref the resolver cannot reach main and fails closed, so
        # a missing env line would silently disable every merge-group skip.
        line = "MERGE_GROUP_BASE_REF: ${{ github.event.merge_group.base_ref }}"
        classify_job = job_block(self.text, "classify")
        classify_step = classify_job[classify_job.index("      - id: classify"):]
        self.assertIn(line, classify_step)
        bootstrap = job_block(self.text, "macos-merge-group")
        self.assertIn(line, bootstrap)
        self.assertIn("bash tools/ci/macos_merge_group_bootstrap.sh", bootstrap)
        self.assertIn(
            "classify_base: ${{ steps.classify.outputs.classify_base }}", classify_job
        )
        self.assertIn('echo "classify_base=$base"', classify_job)

    def test_receipt_reuse_refuses_a_group_stacked_on_unlanded_entries(self) -> None:
        reuse = job_block(self.text, "protected-receipt-reuse")
        self.assertIn(
            "CLASSIFY_BASE: ${{ needs.classify.outputs.classify_base }}", reuse
        )
        guard = reuse.index('elif [ "$base" != "${CLASSIFY_BASE:-}" ]; then')
        # The guard runs before the verifier is loaded from the group's first
        # parent, so a speculative group's tools/ never act as the verifier.
        self.assertLess(
            guard, reuse.index('git show "$base:tools/scripts/protected_merge_receipt.py"')
        )

    def test_classify_uses_bounded_exact_tree_checkout(self) -> None:
        classify_job = job_block(self.text, "classify")
        self.assertIn("fetch-depth: 1", classify_job)
        self.assertNotIn("fetch-depth: 0", classify_job)
        self.assertIn('git fetch --no-tags --depth=1 origin "$base"', classify_job)
        self.assertIn("--mode=diff --comparison=trees", classify_job)
        self.assertIn(
            '"$ci_python" tools/scripts/classify_changes.py --mode=files',
            classify_job,
        )
        self.assertIn(
            "Never trust a retained remote-tracking ref", classify_job
        )

    def test_classify_resolves_one_python311_runtime_under_launchd_path(self) -> None:
        classify_job = job_block(self.text, "classify")

        self.assertIn(
            'export PATH="/opt/homebrew/bin:/usr/local/bin:$HOME/.local/bin:$PATH"',
            classify_job,
        )
        self.assertIn(
            'ci_python="$(python3 tools/ci/find_python311.py)"', classify_job
        )
        for invocation in (
            '"$ci_python" tools/scripts/resolve_classify_base.py',
            '"$ci_python" tools/scripts/classify_changes.py',
            '"$ci_python" -c',
            '"$ci_python" "$trusted_bump_check"',
        ):
            with self.subTest(invocation=invocation):
                self.assertIn(invocation, classify_job)
        self.assertNotIn(
            "python3 tools/scripts/classify_changes.py",
            classify_job,
            "a bare parser invocation would restore machine-dependent tomllib failures",
        )
        self.assertNotIn(
            "python3 tools/scripts/resolve_classify_base.py", classify_job
        )
        self.assertNotIn("python3 -c", classify_job)

    def test_classify_no_longer_hardcodes_origin_main_as_the_base(self) -> None:
        # The dead inline expression this replaces.
        self.assertNotIn(
            "github.event.pull_request.base.sha || 'origin/main'", self.text
        )

    def test_generated_bump_override_executes_only_protected_base_code(self) -> None:
        classify_job = job_block(self.text, "classify")
        self.assertIn(
            'git show "$trusted_verifier_base:tools/scripts/generated_version_bump_check.py"',
            classify_job,
        )
        self.assertIn('trusted_verifier_base="$PR_BASE_SHA"', classify_job)
        self.assertIn('[ "$base" = "$PR_BASE_SHA" ]', classify_job)
        self.assertIn('[ "$base" = "$MERGE_GROUP_BASE_SHA" ]', classify_job)
        self.assertNotIn("mapfile", classify_job)
        self.assertEqual(
            classify_job.count("while IFS= read -r parent; do"),
            3,
            "the macOS-routed classifier must collect each parent list with Bash 3 syntax",
        )
        for name in ("group", "candidate", "cumulative"):
            self.assertIn(f'{name}_parent_count=0', classify_job)
            self.assertIn(f'[ "${name}_parent_count" -eq ', classify_job)
            self.assertNotIn(f'${{#{name}_parents[@]}}', classify_job)
        self.assertIn('[ "${group_parents[0]}" = "$base" ]', classify_job)
        self.assertIn(
            '[ "${cumulative_parents[0]}" = "$candidate_base" ]',
            classify_job,
        )
        self.assertNotIn(
            'git show "$base:tools/scripts/generated_version_bump_check.py"',
            classify_job,
        )
        self.assertIn(
            'GH_TOKEN="${{ github.token }}" "$ci_python" "$trusted_bump_check"',
            classify_job,
        )
        self.assertNotIn("          GH_TOKEN: ${{ github.token }}", classify_job)
        self.assertIn("native_build_required=false", classify_job)
        self.assertIn(
            'echo "native_build_required=$native_build_required"',
            classify_job,
        )
        self.assertIn('} >> "$GITHUB_OUTPUT"', classify_job)

    def test_windows_functional_matrix_uses_the_stable_runtime_image(self) -> None:
        self.assertIn(
            '"--github-hosted-label", "windows-2022"', self.text
        )
        self.assertNotIn(
            '"--github-hosted-label", "windows-latest"', self.text
        )
        # Newest-toolchain coverage remains explicit in the three standalone
        # compile gates, where an image migration cannot invalidate runtime
        # behavior across the entire Windows test suite.
        self.assertEqual(self.text.count("runs-on: windows-latest"), 3)
        for job_name in (
            "windows-msvc-release-gate",
            "windows-midi2-gate",
            "windows-ble-gate",
        ):
            with self.subTest(job=job_name):
                marker = f"\n  {job_name}:\n"
                self.assertIn(marker, self.text)
                remainder = self.text.split(marker, 1)[1]
                job = re.split(r"\n  [A-Za-z0-9_-]+:\n", remainder, 1)[0]
                self.assertIn("runs-on: windows-latest", job)

        # The profile planner is currently read-only, but its inspected PR
        # policy must not contradict the workflow selector that dispatches.
        # Coverage and scheduled validation deliberately retain latest.
        shipyard_profile = SHIPYARD_PROFILE.read_text(encoding="utf-8")
        pr_policy = shipyard_profile.split(
            '[repo."Generous-Corp/pulp".pr.windows]', 1
        )[1].split("\n[", 1)[0]
        self.assertIn('targets = ["github.windows-x64-runtime"]', pr_policy)

        runtime_target = shipyard_profile.split(
            '[targets."github.windows-x64-runtime"]', 1
        )[1].split("\n[", 1)[0]
        self.assertIn('runs_on_json = "windows-2022"', runtime_target)

        latest_target = shipyard_profile.split(
            '[targets."github.windows-x64"]', 1
        )[1].split("\n[", 1)[0]
        self.assertIn('runs_on_json = "windows-latest"', latest_target)

    def test_workflow_triggers_on_push_to_main(self) -> None:
        # Without this trigger the cache-save steps below are unreachable:
        # the workflow simply never runs on a main-branch push.
        self.assertRegex(self.text, r"(?m)^  push:\n    branches: \[main\]$")

    def test_push_runs_are_not_cancelled_by_concurrency(self) -> None:
        # Cancelling a superseded main run throws away the cache it exists to
        # publish. PR runs must still cancel; a local proof neither cancels nor
        # is cancelled.
        match = re.search(r"^  cancel-in-progress: \$\{\{ (.*) \}\}$", self.text, re.MULTILINE)
        self.assertIsNotNone(match, "concurrency must compute cancel-in-progress")
        expression = match.group(1)
        self.assertEqual(
            [term.strip() for term in expression.split("&&")],
            ["github.event_name != 'push'", "!inputs.local_proof"],
        )


class CacheSaveScopeTests(unittest.TestCase):
    """The cache-save steps must fire on main pushes, and only off macOS.

    The local self-hosted Macs keep ccache + FetchContent on disk between
    jobs. Round-tripping those multi-GB caches through GitHub's cloud cache
    would slow down the one required gate in the repo to no benefit. The macOS
    leg still RUNS on a push, as main's health detector; it just saves nothing.
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

    def test_push_matrix_omits_the_macos_leg(self) -> None:
        # A push run publishes the Linux/Windows caches and nothing else. The
        # merge group's required `macos` job already tested the commit that
        # lands; a push leg on the self-hosted selector queued unserved while
        # holding main's push concurrency group. tools/ci/
        # test_build_matrix_contract.py asserts the guard over the parsed
        # syntax tree, and tools/scripts/test_fork_pr_runner_routing.py runs
        # the resolver on a push event.
        self.assertIn('if EVENT_NAME != "push":', self.text)
        self.assertNotIn("PUSH_ONLY_CACHE_EVENTS", self.text)

    def test_push_macos_leg_does_not_claim_the_required_context(self) -> None:
        # The matrix child renames itself to the required `macos` context only
        # for the events whose branch protection consumes it.
        self.assertIn(
            "(github.event_name == 'pull_request' || github.event_name == "
            "'workflow_dispatch' || github.event_name == 'merge_group') && "
            "matrix.key == 'macos' && 'macos'",
            self.text,
        )

    def test_reporting_aliases_and_extra_gates_skip_push_runs(self) -> None:
        # Linux/Windows aliases skip cache-warming pushes. Native macOS now
        # reports directly from its matrix child; its bootstrap is limited to
        # PR and Shipyard workflow-dispatch validation.
        for alias in ("linux", "windows"):
            with self.subTest(alias=alias):
                self.assertIn(
                    "github.event_name != 'push'", job_condition(self.text, alias)
                )
        self.assertIn(
            "&& (github.event_name == 'pull_request' || github.event_name == 'workflow_dispatch')",
            job_condition(self.text, "macos"),
        )
        # windows-msvc-release-gate, windows-midi2-gate, windows-ble-gate, and
        # every other windows-latest job: a new one must not silently start
        # running on cache-warming pushes.
        windows_jobs = jobs_on(self.text, "windows-latest")
        self.assertEqual(len(windows_jobs), 3, windows_jobs)
        for job in windows_jobs:
            with self.subTest(job=job):
                condition = job_condition(self.text, job)
                self.assertIn("needs.classify.outputs.native_build_required == 'true'", condition)
                self.assertIn("github.event_name != 'push'", condition)


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


def _sha(n: int) -> str:
    return f"{n + 0xabc:040x}"  # never the all-zero sha


class MergeGroupMainBaseTests(unittest.TestCase):
    """The first-parent walk from merge_group.base_sha down to main's tip."""

    def _chain(self, length: int) -> dict[str, str]:
        # _sha(length) -> ... -> _sha(1) -> _sha(0) == main
        return {_sha(i): _sha(i - 1) for i in range(1, length + 1)}

    def test_group_directly_on_main_returns_main(self) -> None:
        self.assertEqual(merge_group_main_base(_sha(0), _sha(0), {}.get), _sha(0))

    def test_stacked_group_walks_down_to_main(self) -> None:
        chain = self._chain(3)
        self.assertEqual(merge_group_main_base(_sha(3), _sha(0), chain.get), _sha(0))

    def test_main_that_moved_onto_an_earlier_group_is_its_base(self) -> None:
        # An earlier group passed and main fast-forwarded to it: the entries
        # up to there are validated on main, only the rest are this group's.
        chain = self._chain(3)
        self.assertEqual(merge_group_main_base(_sha(3), _sha(2), chain.get), _sha(2))

    def test_unreachable_main_fails_closed(self) -> None:
        chain = self._chain(3)
        self.assertIsNone(merge_group_main_base(_sha(3), "f" * 40, chain.get))

    def test_unreadable_commit_fails_closed(self) -> None:
        self.assertIsNone(merge_group_main_base(_sha(3), _sha(0), lambda _: None))

    def test_walk_is_bounded(self) -> None:
        chain = self._chain(MAX_QUEUE_WALK + 1)
        self.assertIsNone(
            merge_group_main_base(_sha(MAX_QUEUE_WALK + 1), _sha(0), chain.get)
        )
        self.assertEqual(
            merge_group_main_base(_sha(MAX_QUEUE_WALK), _sha(0), chain.get),
            _sha(0),
        )

    def test_missing_or_malformed_shas_fail_closed(self) -> None:
        for group, main in (("", _sha(0)), (_sha(0), ""), (ZERO_SHA, ZERO_SHA),
                            ("not-a-sha", _sha(0)), (None, None)):
            with self.subTest(group=group, main=main):
                self.assertIsNone(merge_group_main_base(group, main, {}.get))


class StackedMergeGroupIntegrationTests(unittest.TestCase):
    """A real merge queue's shape, driven through the resolver CLI + classifier.

    Main holds docs only. Entry A changes C++; entry B changes docs. GitHub
    builds A's group as merge(main, A) and B's as merge(A's group, B), so B's
    `merge_group.base_sha` is A's speculative group. B's group passing lands
    A, so B's classification must see A's C++ change.
    """

    def _git(self, cwd: Path, *args: str) -> str:
        proc = subprocess.run(
            ["git", *args], cwd=cwd, capture_output=True, text=True, check=True
        )
        return proc.stdout.strip()

    def setUp(self) -> None:
        import tempfile

        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.remote = self.root / "remote.git"
        src = self.root / "source"
        subprocess.run(["git", "init", "-q", "--bare", str(self.remote)], check=True)
        subprocess.run(["git", "clone", "-q", str(self.remote), str(src)],
                       check=True, capture_output=True)
        self.src = src
        for key, value in (("user.email", "ci@example.invalid"), ("user.name", "ci")):
            self._git(src, "config", key, value)
        self._git(src, "symbolic-ref", "HEAD", "refs/heads/main")
        (src / "docs").mkdir()
        (src / "docs" / "README.md").write_text("seed\n", encoding="utf-8")
        self._git(src, "add", "docs/README.md")
        self._git(src, "commit", "-qm", "seed")
        self.main = self._git(src, "rev-parse", "HEAD")
        self._git(src, "push", "-q", "origin", "main")

        # Entry A: C++. Entry B: docs only. Both branch from main.
        self._git(src, "checkout", "-qb", "entry-a", self.main)
        (src / "core").mkdir()
        (src / "core" / "x.cpp").write_text("int x;\n", encoding="utf-8")
        self._git(src, "add", "core/x.cpp")
        self._git(src, "commit", "-qm", "fix(format): C++ entry")
        self.entry_a = self._git(src, "rev-parse", "HEAD")
        self._git(src, "checkout", "-qb", "entry-b", self.main)
        (src / "docs" / "guide.md").write_text("docs entry\n", encoding="utf-8")
        self._git(src, "add", "docs/guide.md")
        self._git(src, "commit", "-qm", "docs: docs entry")
        self.entry_b = self._git(src, "rev-parse", "HEAD")

    def _group(self, base: str, entry: str, number: int) -> str:
        """GitHub's speculative group commit: merge(base, entry) on a queue ref."""
        self._git(self.src, "checkout", "-q", "--detach", base)
        self._git(self.src, "merge", "-q", "--no-ff", "--no-edit", entry)
        group = self._git(self.src, "rev-parse", "HEAD")
        self._git(self.src, "push", "-q", "origin",
                  f"{group}:refs/heads/gh-readonly-queue/main/pr-{number}-{base}")
        return group

    def _checkout(self, sha: str) -> Path:
        """actions/checkout at fetch-depth 1: one commit, no history."""
        work = self.root / f"checkout-{sha[:8]}"
        work.mkdir()
        self._git(work, "init", "-q")
        self._git(work, "remote", "add", "origin", self.remote.as_uri())
        self._git(work, "fetch", "-q", "--no-tags", "--depth=1", "origin", sha)
        self._git(work, "checkout", "-q", "--detach", sha)
        return work

    def _resolve(self, work: Path, group_base: str, base_ref: str = "refs/heads/main") -> str:
        environ = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
        environ.update(GITHUB_EVENT_NAME="merge_group",
                       MERGE_GROUP_BASE_SHA=group_base,
                       MERGE_GROUP_BASE_REF=base_ref)
        proc = subprocess.run([sys.executable, str(SCRIPT)], cwd=work, env=environ,
                              capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return proc.stdout.strip()

    def _classify(self, work: Path, base: str) -> dict:
        import json

        self._git(work, "fetch", "-q", "--no-tags", "--depth=1", "origin", base)
        proc = subprocess.run(
            [sys.executable, str(REPO_ROOT / "tools" / "scripts" / "classify_changes.py"),
             "--mode=diff", "--comparison=trees", "--base", base, "--json"],
            cwd=work, env={**os.environ, "GITHUB_OUTPUT": ""},
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return json.loads(proc.stdout)

    def test_docs_entry_behind_a_cpp_entry_requires_the_native_build(self) -> None:
        group_a = self._group(self.main, self.entry_a, 1)
        group_b = self._group(group_a, self.entry_b, 2)
        work = self._checkout(group_b)

        base = self._resolve(work, group_a)
        self.assertEqual(base, self.main, "a stacked group must diff against main")
        result = self._classify(work, base)
        self.assertTrue(result["native_build_required"], result)
        self.assertIn("core/x.cpp", result["reason"])

        # Negative control: the speculative base the workflow used to diff
        # against sees only B's docs and skips the build that lands A.
        stale = self._classify(work, group_a)
        self.assertFalse(stale["native_build_required"], stale)

    def test_docs_only_group_directly_on_main_still_skips(self) -> None:
        group_b = self._group(self.main, self.entry_b, 2)
        work = self._checkout(group_b)
        base = self._resolve(work, self.main)
        self.assertEqual(base, self.main)
        result = self._classify(work, base)
        self.assertFalse(result["native_build_required"], result)
        self.assertEqual(result["changed_file_count"], 1)

    def test_group_whose_entries_ahead_already_landed_diffs_the_new_main(self) -> None:
        group_a = self._group(self.main, self.entry_a, 1)
        group_b = self._group(group_a, self.entry_b, 2)
        # A's group passed and main fast-forwarded to it.
        self._git(self.src, "push", "-q", "origin", f"{group_a}:refs/heads/main")
        work = self._checkout(group_b)
        base = self._resolve(work, group_a)
        self.assertEqual(base, group_a)
        self.assertFalse(self._classify(work, base)["native_build_required"])

    def test_main_off_the_chain_or_no_base_ref_fails_closed(self) -> None:
        group_a = self._group(self.main, self.entry_a, 1)
        group_b = self._group(group_a, self.entry_b, 2)
        work = self._checkout(group_b)
        self.assertEqual(self._resolve(work, group_a, base_ref=""), DEFAULT_BASE)
        self.assertEqual(
            self._resolve(work, group_a, base_ref="refs/heads/no-such-branch"),
            DEFAULT_BASE,
        )
        # Main rewritten to an unrelated commit: the chain never reaches it.
        self._git(self.src, "checkout", "-q", "--orphan", "unrelated")
        self._git(self.src, "commit", "-q", "--allow-empty", "-m", "unrelated")
        unrelated = self._git(self.src, "rev-parse", "HEAD")
        self._git(self.src, "push", "-q", "-f", "origin", f"{unrelated}:refs/heads/main")
        self.assertEqual(self._resolve(work, group_a), DEFAULT_BASE)


class ShallowExactTreeIntegrationTests(unittest.TestCase):
    """A distant pinned base works without fetching intervening history."""

    def test_two_exact_shallow_commits_are_sufficient(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            remote = root / "remote.git"
            source = root / "source"
            shallow = root / "shallow"
            subprocess.run(
                ["git", "init", "-q", "--bare", str(remote)], check=True
            )
            subprocess.run(
                ["git", "clone", "-q", str(remote), str(source)], check=True
            )
            for key, value in (("user.email", "ci@example.invalid"),
                               ("user.name", "ci")):
                subprocess.run(
                    ["git", "config", key, value], cwd=source, check=True
                )
            subprocess.run(
                ["git", "symbolic-ref", "HEAD", "refs/heads/main"],
                cwd=source,
                check=True,
            )
            (source / "README.md").write_text("base\n", encoding="utf-8")
            subprocess.run(["git", "add", "README.md"], cwd=source, check=True)
            subprocess.run(
                ["git", "commit", "-qm", "base"], cwd=source, check=True
            )
            base = subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=source, text=True
            ).strip()
            (source / "middle.txt").write_text("unneeded history\n", encoding="utf-8")
            subprocess.run(["git", "add", "middle.txt"], cwd=source, check=True)
            subprocess.run(
                ["git", "commit", "-qm", "middle"], cwd=source, check=True
            )
            (source / "core").mkdir()
            (source / "core" / "x.cpp").write_text("int x;\n", encoding="utf-8")
            subprocess.run(["git", "add", "core/x.cpp"], cwd=source, check=True)
            subprocess.run(
                ["git", "commit", "-qm", "head"], cwd=source, check=True
            )
            subprocess.run(
                ["git", "push", "-q", "origin", "main"], cwd=source, check=True
            )
            subprocess.run(
                [
                    "git", "clone", "-q", "--depth=1", "--branch", "main",
                    remote.as_uri(), str(shallow),
                ],
                check=True,
            )
            subprocess.run(
                ["git", "fetch", "-q", "--no-tags", "--depth=1", "origin", base],
                cwd=shallow,
                check=True,
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(REPO_ROOT / "tools" / "scripts" / "classify_changes.py"),
                    "--mode=diff", "--comparison=trees", "--base", base, "--json",
                ],
                cwd=shallow,
                capture_output=True,
                text=True,
                check=True,
            )
            payload = __import__("json").loads(result.stdout)
            self.assertTrue(payload["native_build_required"])
            self.assertEqual(payload["changed_file_count"], 2)
            self.assertIn("core/x.cpp", payload["reason"])
            for ref in ("HEAD", base):
                self.assertEqual(
                    subprocess.check_output(
                        ["git", "rev-list", "--count", ref],
                        cwd=shallow,
                        text=True,
                    ).strip(),
                    "1",
                )
            self.assertNotEqual(
                subprocess.run(
                    ["git", "merge-base", base, "HEAD"],
                    cwd=shallow,
                    capture_output=True,
                    check=False,
                ).returncode,
                0,
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
