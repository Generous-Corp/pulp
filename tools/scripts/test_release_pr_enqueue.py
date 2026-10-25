#!/usr/bin/env python3
"""Tests for release_pr_enqueue.py.

Load-bearing behavior: a release PR enters the merge queue only when every
required check on its head passed AND the queue holds nothing but release PRs;
the max-wait backstop and a manual `force` bypass the idle test but never the
green one; a draft, a conflicting PR or one already queued is left alone; at
most one release PR is queued at a time; release PRs are recognized by exact
branch and label; immediately before the enqueue the script runs main's
release-PR proof on the exact head it will name, and a refused proof or a head
that moved means no enqueue; the enqueue is bound to that head; and in Actions
it runs only from main on a push, schedule or dispatch.
"""
from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import release_pr_enqueue as rpe  # noqa: E402

NOW = datetime(2026, 10, 24, 18, 0, tzinfo=timezone.utc)
SDK_BRANCH = "release-please--branches--main"
PLUGIN_BRANCH = "release-please--branches--main--components--plugin"


def release_pr(hours_open: float = 1.0, **extra) -> dict:
    opened = (NOW - timedelta(hours=hours_open)).isoformat().replace("+00:00", "Z")
    return {"number": 5, "headRefName": SDK_BRANCH, "isCrossRepository": False,
            "createdAt": opened, "isDraft": False, "mergeable": "MERGEABLE",
            "mergeQueueEntry": None, **extra}


def labelled(*names: str) -> dict:
    return {"labels": {"nodes": [{"name": n} for n in names]}}


def entry(branch: str) -> dict:
    return {"state": "QUEUED", "pullRequest": {"number": 1, "headRefName": branch}}


GREEN = (True, "3 required checks green")


class Decide(unittest.TestCase):
    def decide(self, pull, queue, green=GREEN, force=False):
        return rpe.decide(pull, queue, green, NOW, 4.0, force)

    def test_green_into_an_empty_queue_enqueues(self) -> None:
        self.assertEqual(self.decide(release_pr(), []).action, "enqueue")

    def test_only_one_release_pr_is_queued_at_a_time(self) -> None:
        for pull, force in ((release_pr(), False), (release_pr(hours_open=9), False),
                            (release_pr(), True)):
            with self.subTest(force=force):
                decision = self.decide(pull, [entry(PLUGIN_BRANCH)], force=force)
                self.assertEqual(decision.action, "wait")
                self.assertIn("one at a time", decision.reason)

    def test_a_real_pr_in_the_queue_makes_it_wait(self) -> None:
        decision = self.decide(release_pr(), [entry("fix/x")])
        self.assertEqual(decision.action, "wait")
        self.assertIn("1 PRs queued", decision.reason)

    def test_the_backstop_enqueues_into_a_busy_queue(self) -> None:
        decision = self.decide(release_pr(hours_open=4.5), [entry("fix/x")])
        self.assertEqual(decision.action, "enqueue")
        self.assertIn("backstop", decision.reason)

    def test_force_enqueues_into_a_busy_queue(self) -> None:
        self.assertEqual(self.decide(release_pr(), [entry("fix/x")], force=True).action, "enqueue")

    def test_nothing_bypasses_red_checks(self) -> None:
        red = (False, "required check macos concluded FAILURE")
        for pull, force in ((release_pr(), False), (release_pr(hours_open=9), False),
                            (release_pr(), True)):
            with self.subTest(force=force):
                self.assertEqual(self.decide(pull, [], red, force).action, "wait")

    def test_held_conflicting_or_queued_prs_are_skipped(self) -> None:
        for extra in ({"isDraft": True}, {"mergeable": "CONFLICTING"},
                      {"mergeQueueEntry": {"state": "QUEUED"}}):
            with self.subTest(extra=extra):
                self.assertEqual(self.decide(release_pr(**extra), []).action, "skip")


class RequiredChecks(unittest.TestCase):
    def test_only_required_contexts_decide(self) -> None:
        contexts = [
            {"__typename": "CheckRun", "name": "macos", "status": "COMPLETED",
             "conclusion": "SUCCESS", "isRequired": True},
            {"__typename": "CheckRun", "name": "coverage", "status": "COMPLETED",
             "conclusion": "FAILURE", "isRequired": False},
            {"__typename": "StatusContext", "context": "ext", "state": "SUCCESS",
             "isRequired": True},
        ]
        self.assertTrue(rpe.required_checks_green(contexts)[0])

    def test_pending_failed_or_absent_required_checks_are_not_green(self) -> None:
        for contexts in (
            [],
            [{"__typename": "CheckRun", "name": "macos", "status": "IN_PROGRESS",
              "conclusion": None, "isRequired": True}],
            [{"__typename": "CheckRun", "name": "macos", "status": "COMPLETED",
              "conclusion": "CANCELLED", "isRequired": True}],
            [{"__typename": "StatusContext", "context": "ext", "state": "PENDING",
              "isRequired": True}],
        ):
            with self.subTest(contexts=contexts):
                self.assertFalse(rpe.required_checks_green(contexts)[0])

    def test_surface_comes_from_exactly_one_label(self) -> None:
        self.assertEqual(rpe.surface_of(labelled("autorelease: pending", "release: sdk")), "sdk")
        self.assertIsNone(rpe.surface_of(labelled("autorelease: pending")))
        self.assertIsNone(rpe.surface_of(labelled("release: sdk", "release: plugin")))

    def test_release_prs_are_recognized_by_exact_branch_label_and_origin(self) -> None:
        def pull(branch, *labels, cross=False):
            return {"headRefName": branch, "isCrossRepository": cross, **labelled(*labels)}
        self.assertTrue(rpe.is_release_pr(pull(PLUGIN_BRANCH, "release: plugin")))
        self.assertTrue(rpe.is_release_pr(pull(SDK_BRANCH, "release: sdk")))
        for other in (pull(SDK_BRANCH, "release: sdk", cross=True),
                      pull(SDK_BRANCH + "-x", "release: sdk"),
                      pull(SDK_BRANCH + "--components--other", "release: sdk"),
                      pull(PLUGIN_BRANCH, "release: sdk"),
                      pull(SDK_BRANCH),
                      pull("release/version-bump", "release: sdk")):
            with self.subTest(other=other):
                self.assertFalse(rpe.is_release_pr(other))

    def test_a_lookalike_branch_in_the_queue_counts_as_a_real_pr(self) -> None:
        decision = rpe.decide(release_pr(), [entry(SDK_BRANCH + "-x")], GREEN, NOW, 4.0, False)
        self.assertEqual(decision.action, "wait")


class Frozen(unittest.TestCase):
    def test_queued_old_or_green_release_prs_are_frozen(self) -> None:
        red = (False, "required check macos is IN_PROGRESS")
        self.assertTrue(rpe.frozen(release_pr(mergeQueueEntry={"state": "QUEUED"}), red, NOW, 4.0)[0])
        self.assertTrue(rpe.frozen(release_pr(hours_open=5), red, NOW, 4.0)[0])
        self.assertTrue(rpe.frozen(release_pr(), GREEN, NOW, 4.0)[0])
        self.assertFalse(rpe.frozen(release_pr(), red, NOW, 4.0)[0])


REPO = "Generous-Corp/pulp"
HEAD = "a" * 40


class StatusContexts(unittest.TestCase):
    REQUIRED = {"__typename": "CheckRun", "name": "macos", "status": "COMPLETED",
                "conclusion": "SUCCESS", "isRequired": True}

    def test_a_status_named_like_a_check_is_judged_as_a_status(self) -> None:
        status = {"__typename": "StatusContext", "context": "macos", "state": "PENDING",
                  "status": "COMPLETED", "conclusion": "SUCCESS", "isRequired": True}
        self.assertFalse(rpe.required_checks_green([status])[0])
        run = {"__typename": "CheckRun", "name": "macos", "status": "COMPLETED",
               "conclusion": "SUCCESS", "state": "FAILURE", "isRequired": True}
        self.assertTrue(rpe.required_checks_green([run])[0])

    def test_a_check_run_must_be_completed_and_a_status_successful(self) -> None:
        for bad in ({**self.REQUIRED, "status": "QUEUED"},
                    {**self.REQUIRED, "conclusion": "FAILURE"},
                    {"__typename": "StatusContext", "context": "ext", "state": "ERROR",
                     "isRequired": True}):
            with self.subTest(bad=bad):
                self.assertFalse(rpe.required_checks_green([bad])[0])
        self.assertTrue(rpe.required_checks_green([
            {**self.REQUIRED, "conclusion": "SKIPPED"},
            {"__typename": "StatusContext", "context": "ext", "state": "SUCCESS",
             "isRequired": True}])[0])


class FakeGitHub(rpe.GitHub):
    def __init__(self, pages: list[dict]) -> None:
        super().__init__(REPO)
        self.pages = list(pages)

    def graphql(self, query: str, **variables) -> dict:
        return self.pages.pop(0)


def checks_page(head_ref: str, commit: str, contexts: list[dict]) -> dict:
    return {"repository": {"pullRequest": {
        "headRefOid": head_ref,
        "commits": {"nodes": [{"commit": {"oid": commit, "statusCheckRollup": {"contexts": {
            "pageInfo": {"hasNextPage": False, "endCursor": None}, "nodes": contexts}}}}]}}}}


class ReadingAndEnqueueing(unittest.TestCase):
    CONTEXTS = [StatusContexts.REQUIRED]

    def test_a_moved_head_is_never_green(self) -> None:
        for head_ref, commit in ((HEAD, "b" * 40), ("b" * 40, HEAD)):
            with self.subTest(head_ref=head_ref, commit=commit):
                github = FakeGitHub([checks_page(head_ref, commit, self.CONTEXTS)])
                ok, why = github.checks(5, HEAD)
                self.assertFalse(ok)
                self.assertIn("head moved", why)
        self.assertTrue(FakeGitHub([checks_page(HEAD, HEAD, self.CONTEXTS)]).checks(5, HEAD)[0])

    def test_the_enqueue_is_bound_to_the_head_that_was_read(self) -> None:
        sent = {}

        class Recorder(FakeGitHub):
            def graphql(self, query, **variables):
                sent.update(variables)
                return {"enqueuePullRequest": {"mergeQueueEntry": {"state": "QUEUED"}}}

        Recorder([]).enqueue("PR_id", HEAD)
        self.assertEqual(sent, {"id": "PR_id", "head": HEAD})
        self.assertIn("expectedHeadOid: $head", rpe._ENQUEUE)

    def run_main(self, proof: tuple[bool, str], env: dict | None = None) -> list:
        pull = {**release_pr(), **labelled("autorelease: pending", "release: sdk"),
                "id": "PR_id", "title": "chore(main): release 1.12.0", "headRefOid": HEAD}
        calls = []

        class Main(FakeGitHub):
            def listing(self):
                return [], [pull]

            def checks(self, number, head):
                calls.append(("checks", head))
                return True, "green"

            def enqueue(self, pull_id, head):
                calls.append(("enqueue", pull_id, head))
                return {}

        def fake_prove(repo, repo_name, listed, head, script, run=None, read_token=None):
            calls.append(("prove", head, script))
            return proof

        originals = rpe.GitHub, rpe.prove_head
        rpe.GitHub = lambda *_args, **_kwargs: Main([])
        rpe.prove_head = fake_prove
        saved = dict(rpe.os.environ)
        rpe.os.environ.clear()
        rpe.os.environ.update(env or {})
        try:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.code = rpe.main(["--repo-name", REPO, "--proof-script", "proof.py"])
        finally:
            rpe.GitHub, rpe.prove_head = originals
            rpe.os.environ.clear()
            rpe.os.environ.update(saved)
        return calls

    def test_a_passing_proof_enqueues_the_proven_head(self) -> None:
        self.assertEqual(self.run_main((True, "passed")),
                         [("checks", HEAD), ("prove", HEAD, "proof.py"), ("enqueue", "PR_id", HEAD)])

    def test_a_refused_proof_or_a_moved_head_means_no_enqueue(self) -> None:
        for why in ("release-PR proof refused: x", "head moved before the proof"):
            with self.subTest(why=why):
                calls = self.run_main((False, why))
                self.assertNotIn("enqueue", [call[0] for call in calls])

    def test_a_waiting_decision_neither_proves_nor_enqueues(self) -> None:
        original = rpe.decide
        rpe.decide = lambda *_args, **_kwargs: rpe.Decision(5, "wait", "busy")
        try:
            self.assertEqual(self.run_main((True, "passed")), [("checks", HEAD)])
        finally:
            rpe.decide = original

    def test_only_main_on_a_trusted_event_may_enqueue(self) -> None:
        for env in ({"GITHUB_ACTIONS": "true", "GITHUB_REF": "refs/heads/feature",
                     "GITHUB_EVENT_NAME": "push"},
                    {"GITHUB_ACTIONS": "true", "GITHUB_REF": "refs/heads/main",
                     "GITHUB_EVENT_NAME": "pull_request_target"},
                    {"GITHUB_ACTIONS": "true", "GITHUB_REF": "refs/pull/1/merge",
                     "GITHUB_EVENT_NAME": "pull_request"}):
            with self.subTest(env=env):
                self.assertEqual(self.run_main((True, "passed"), env), [])
                self.assertEqual(self.code, 1)
        trusted = {"GITHUB_ACTIONS": "true", "GITHUB_REF": "refs/heads/main",
                   "GITHUB_EVENT_NAME": "schedule"}
        self.assertIn(("enqueue", "PR_id", HEAD),
                      self.run_main((True, "passed"), trusted))


class GraphQLTransport(unittest.TestCase):
    def call(self, proc: subprocess.CompletedProcess, **variables):
        seen = {}

        def fake_run(argv, **kwargs):
            seen["argv"] = argv
            return proc

        original = rpe.subprocess.run
        rpe.subprocess.run = fake_run
        try:
            return rpe.GitHub(REPO).graphql("query", **variables), seen
        finally:
            rpe.subprocess.run = original

    def test_variables_are_typed_and_none_is_omitted(self) -> None:
        ok = subprocess.CompletedProcess([], 0, json.dumps({"data": {"x": 1}}), "")
        data, seen = self.call(ok, number=5, cursor=None, owner="o")
        self.assertEqual(data, {"x": 1})
        argv = seen["argv"]
        self.assertIn("number=5", argv)
        self.assertEqual(argv[argv.index("number=5") - 1], "-F")
        self.assertEqual(argv[argv.index("owner=o") - 1], "-f")
        self.assertFalse(any(arg.startswith("cursor=") for arg in argv))

    def test_a_failed_call_or_graphql_errors_raise(self) -> None:
        for proc in (subprocess.CompletedProcess([], 1, "", "boom"),
                     subprocess.CompletedProcess([], 0, json.dumps({"errors": [{"m": 1}]}), "")):
            with self.subTest(proc=proc), self.assertRaises(rpe.GitHubError):
                self.call(proc)

    def test_listing_tolerates_a_repository_without_a_queue(self) -> None:
        class NoQueue(FakeGitHub):
            def graphql(self, query, **variables):
                return {"repository": {"mergeQueue": None, "pullRequests": {"nodes": [{"n": 1}]}}}
        self.assertEqual(NoQueue([]).listing(), ([], [{"n": 1}]))

    def test_a_head_with_no_rollup_is_not_green(self) -> None:
        page = checks_page(HEAD, HEAD, [])
        page["repository"]["pullRequest"]["commits"]["nodes"][0]["commit"]["statusCheckRollup"] = None
        ok, why = FakeGitHub([page]).checks(5, HEAD)
        self.assertFalse(ok)
        self.assertIn("no checks", why)


class ProveHead(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.origin, self.clone = root / "origin", root / "clone"
        subprocess.run(["git", "init", "-q", "-b", "main", str(self.origin)], check=True)
        self.git(self.origin, "config", "user.email", "t@e")
        self.git(self.origin, "config", "user.name", "t")
        self.git(self.origin, "commit", "-q", "--allow-empty", "-m", "base")
        self.base = self.git(self.origin, "rev-parse", "HEAD")
        self.git(self.origin, "checkout", "-q", "-b", SDK_BRANCH)
        self.git(self.origin, "commit", "-q", "--allow-empty", "-m", "chore(main): release 1.12.0")
        self.release = self.git(self.origin, "rev-parse", "HEAD")
        self.git(self.origin, "commit", "-q", "--allow-empty", "-m",
                 "chore: refresh release-derived files")
        self.head = self.git(self.origin, "rev-parse", "HEAD")
        subprocess.run(["git", "clone", "-q", str(self.origin), str(self.clone)], check=True)
        self.pull = {"number": 5, "title": "chore(main): release 1.12.0", "body": "notes",
                     "headRefName": SDK_BRANCH,
                     **labelled("autorelease: pending", "release: sdk")}

    def tearDown(self) -> None:
        self._tmp.cleanup()

    @staticmethod
    def git(repo: Path, *args: str) -> str:
        return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True,
                              text=True, encoding="utf-8").stdout.strip()

    def prove(self, head: str, returncode: int = 0):
        seen = {}

        def fake_run(argv, **kwargs):
            seen["argv"] = argv
            seen["event"] = json.loads(Path(argv[argv.index("--event-path") + 1])
                                       .read_text(encoding="utf-8"))
            return subprocess.CompletedProcess(argv, returncode, "", "generated-version-bump: "
                                               "full validation (no)\n")
        return rpe.prove_head(str(self.clone), REPO, self.pull, head, "proof.py",
                              run=fake_run), seen

    def test_the_proof_runs_main_s_checker_on_the_exact_head(self) -> None:
        (ok, _why), seen = self.prove(self.head)
        self.assertTrue(ok)
        argv = seen["argv"]
        self.assertEqual(argv[1], "proof.py")
        self.assertEqual(argv[argv.index("--head") + 1], self.head)
        self.assertEqual(argv[argv.index("--base") + 1], self.base)
        pull = seen["event"]["pull_request"]
        self.assertEqual((pull["head"]["sha"], pull["base"]["sha"], pull["commits"]),
                         (self.head, self.base, 2))
        self.assertEqual([label["name"] for label in pull["labels"]],
                         ["autorelease: pending", "release: sdk"])

    def test_a_refused_proof_is_reported(self) -> None:
        (ok, why), _seen = self.prove(self.head, returncode=1)
        self.assertFalse(ok)
        self.assertIn("proof refused", why)

    def test_the_event_carries_the_pr_body_and_a_single_commit_count(self) -> None:
        self.git(self.origin, "checkout", "-q", "-B", SDK_BRANCH, self.release)
        subprocess.run(["git", "-C", str(self.clone), "fetch", "-q", "origin"], check=True)
        (ok, _why), seen = self.prove(self.release)
        self.assertTrue(ok)
        pull = seen["event"]["pull_request"]
        self.assertEqual((pull["body"], pull["commits"]), ("notes", 1))

    def test_an_unreadable_branch_is_not_proven(self) -> None:
        self.pull = {**self.pull, "headRefName": "no-such-branch"}
        (ok, why), seen = self.prove(self.head)
        self.assertFalse(ok)
        self.assertIn("could not read the head", why)
        self.assertNotIn("argv", seen)

    def test_a_head_that_moved_before_the_proof_is_refused(self) -> None:
        (ok, why), seen = self.prove(self.release)
        self.assertFalse(ok)
        self.assertIn("head moved", why)
        self.assertNotIn("argv", seen)


if __name__ == "__main__":
    unittest.main()
