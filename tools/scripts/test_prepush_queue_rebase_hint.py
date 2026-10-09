#!/usr/bin/env python3
"""Fixture-repo tests for the pre-push queue-rebase hint and queued-PR guard.

Each case builds a real Git repository whose `origin/main` advanced after the
feature branch forked and whose `origin/feature` is the PR's current head, then
feeds Git's pushed-ref record to `prepush_queue_rebase_hint.py` with a fake
`gh` answering the GraphQL lookup. The fake records every call, so a case can
assert that a content-changing push never reaches the network at all.

Positive controls: the rebase-only and merge-only cases must PRINT the hint,
so a hint that silently stopped classifying fails here rather than passing as
"quiet".

The queued-PR guard cases run the real `.githooks/pre-push` in the fixture
with `PULP_SKIP_PREPUSH=1`, so reaching the "skipping gates" line proves the
guard let the push through, and a refusal proves the skip knob does not bypass
the guard.
"""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/scripts/prepush_queue_rebase_hint.py"
HOOK = ROOT / ".githooks/pre-push"
HOOK_LIB = ROOT / ".githooks/lib"

FAKE_GH = r"""#!/usr/bin/env python3
import json, os, sys, time
open(os.environ["FAKE_GH_CALLS"], "a").write(" ".join(sys.argv[1:3]) + "\n")
mode = os.environ.get("FAKE_GH_MODE", "ok")
if mode == "fail":
    print("error connecting to api.github.com", file=sys.stderr); sys.exit(1)
if mode == "hang":
    time.sleep(30)
if mode == "garbage":
    print("<html>not json</html>"); sys.exit(0)
print(open(os.environ["FAKE_GH_FIXTURE"]).read())
"""

ZERO = "0" * 40


def pr_fixture(*, armed=False, queued=False, present=True) -> dict:
    nodes = []
    if present:
        nodes.append(
            {
                "number": 4242,
                "headRefOid": "x",
                "autoMergeRequest": {"enabledAt": "2026-09-29T00:00:00Z"} if armed else None,
                "mergeQueueEntry": {"state": "QUEUED", "position": 1} if queued else None,
            }
        )
    return {"data": {"repository": {"pullRequests": {"nodes": nodes}}}}


class Repo:
    def __init__(self, tmp: Path):
        self.tmp = tmp
        self.dir = tmp / "work"
        self.dir.mkdir()
        self.gh = tmp / "fake-gh"
        self.gh.write_text(FAKE_GH)
        self.gh.chmod(self.gh.stat().st_mode | stat.S_IEXEC)
        self.fixture = tmp / "fixture.json"
        self.calls = tmp / "calls.log"
        self.calls.write_text("")
        self.set_pr(pr_fixture(armed=True))

        self.git("init", "-q", "-b", "main")
        self.commit("a.txt", "a\n", "base")
        self.git("update-ref", "refs/remotes/origin/main", "HEAD")
        self.git("checkout", "-q", "-b", "feature")
        self.commit("feature.txt", "one\n", "feature work")
        self.old_head = self.rev("HEAD")
        self.git("update-ref", "refs/remotes/origin/feature", "HEAD")
        # main advances with an unrelated change after the fork
        self.git("checkout", "-q", "main")
        self.commit("b.txt", "b\n", "main moves on")
        self.git("update-ref", "refs/remotes/origin/main", "HEAD")
        self.git("checkout", "-q", "feature")

    def set_pr(self, data: dict) -> None:
        self.fixture.write_text(json.dumps(data))

    def env(self, **extra: str) -> dict[str, str]:
        env = dict(os.environ)
        for k in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "PULP_ALLOW_QUEUE_REBASE",
                  "PULP_ALLOW_QUEUED_PUSH", "PULP_SKIP_PREPUSH", "PULP_DISABLE_PREPUSH_GATES",
                  "PULP_QUEUE_REBASE_TIMEOUT", "PYTHON"):
            env.pop(k, None)
        env.update(
            GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t",
            GIT_CONFIG_NOSYSTEM="1", HOME=str(self.tmp),
            PULP_QUEUE_REBASE_GH=str(self.gh), FAKE_GH_FIXTURE=str(self.fixture), FAKE_GH_CALLS=str(self.calls),
        )
        env.update(extra)
        return env

    def git(self, *args: str) -> str:
        res = subprocess.run(["git", *args], cwd=self.dir, capture_output=True, text=True, env=self.env())
        if res.returncode != 0:
            raise AssertionError(f"git {' '.join(args)} failed: {res.stderr}")
        return res.stdout.strip()

    def rev(self, rev: str) -> str:
        return self.git("rev-parse", rev)

    def commit(self, rel: str, text: str, msg: str) -> str:
        (self.dir / rel).write_text(text)
        self.git("add", rel)
        self.git("commit", "-q", "-m", msg)
        return self.rev("HEAD")

    def push(self, **env: str) -> subprocess.CompletedProcess:
        record = f"refs/heads/feature {self.rev('HEAD')} refs/heads/feature {self.old_head}\n"
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(self.dir),
             "--remote-url", "git@github.com:Generous-Corp/pulp.git"],
            input=record, capture_output=True, text=True, env=self.env(**env), timeout=20,
        )

    def run_hook(self, **env: str) -> subprocess.CompletedProcess:
        # Mirror only what the hook needs before PULP_SKIP_PREPUSH; the guard
        # and the hint share one script.
        shutil.copytree(HOOK_LIB, self.dir / ".githooks/lib", dirs_exist_ok=True)
        shutil.copy2(HOOK, self.dir / ".githooks/pre-push")
        (self.dir / "tools" / "scripts").mkdir(parents=True, exist_ok=True)
        shutil.copy2(SCRIPT, self.dir / "tools/scripts/prepush_queue_rebase_hint.py")
        record = f"refs/heads/feature {self.rev('HEAD')} refs/heads/feature {self.old_head}\n"
        return subprocess.run(
            ["bash", str(self.dir / ".githooks/pre-push"), "origin",
             "git@github.com:Generous-Corp/pulp.git"],
            cwd=self.dir, input=record, capture_output=True, text=True, encoding="utf-8",
            env=self.env(PYTHON=sys.executable, **env), timeout=60,
        )

    def lookups(self) -> int:
        return len([l for l in self.calls.read_text().splitlines() if l.strip()])


class QueueRebaseHintTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Repo(Path(self._tmp.name))

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def assertHint(self, res: subprocess.CompletedProcess) -> None:
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("PR #4242", res.stderr)
        self.assertIn("shipyard ship --pr 4242", res.stderr)
        self.assertIn("PULP_ALLOW_QUEUE_REBASE=1", res.stderr)

    def assertQuiet(self, res: subprocess.CompletedProcess) -> None:
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stderr.strip(), "")

    def test_rebase_only_on_armed_pr_hints(self) -> None:
        self.repo.git("rebase", "-q", "origin/main")
        self.assertHint(self.repo.push())
        self.assertIn("auto-merge armed", self.repo.push().stderr)

    def test_rebase_only_on_queued_pr_hints(self) -> None:
        self.repo.set_pr(pr_fixture(queued=True))
        self.repo.git("rebase", "-q", "origin/main")
        res = self.repo.push()
        self.assertHint(res)
        self.assertIn("in the merge queue", res.stderr)

    def test_merge_of_main_after_a_rebase_hints(self) -> None:
        # A rebase onto an older main, then a merge of newer main on top: not a
        # plain old-head + merges push, so refresh_push_check does not cover it.
        self.repo.git("rebase", "-q", "origin/main")
        self.repo.git("checkout", "-q", "main")
        self.repo.commit("c.txt", "c\n", "main moves again")
        self.repo.git("update-ref", "refs/remotes/origin/main", "HEAD")
        self.repo.git("checkout", "-q", "feature")
        self.repo.git("merge", "-q", "--no-edit", "origin/main")
        self.assertHint(self.repo.push())

    def test_plain_merge_of_main_is_left_to_refresh_push_check(self) -> None:
        self.repo.git("merge", "-q", "--no-edit", "origin/main")
        self.assertQuiet(self.repo.push())
        self.assertEqual(self.repo.lookups(), 0)

    def test_content_change_does_not_hint_or_look_up(self) -> None:
        self.repo.git("rebase", "-q", "origin/main")
        self.repo.commit("feature.txt", "one\ntwo\n", "more feature work")
        self.assertQuiet(self.repo.push())
        self.assertEqual(self.repo.lookups(), 0)

    def test_amended_content_during_rebase_does_not_hint(self) -> None:
        self.repo.git("rebase", "-q", "origin/main")
        (self.repo.dir / "feature.txt").write_text("changed\n")
        self.repo.git("commit", "-q", "-a", "--amend", "--no-edit")
        self.assertQuiet(self.repo.push())

    def test_history_rewrite_without_base_move_does_not_hint(self) -> None:
        self.repo.git("commit", "-q", "--amend", "-m", "reworded")
        self.assertQuiet(self.repo.push())
        self.assertEqual(self.repo.lookups(), 0)

    def test_not_armed_does_not_hint(self) -> None:
        self.repo.set_pr(pr_fixture())
        self.repo.git("rebase", "-q", "origin/main")
        self.assertQuiet(self.repo.push())
        self.assertEqual(self.repo.lookups(), 1)

    def test_no_open_pr_does_not_hint(self) -> None:
        self.repo.set_pr(pr_fixture(present=False))
        self.repo.git("rebase", "-q", "origin/main")
        self.assertQuiet(self.repo.push())

    def test_network_error_is_silent(self) -> None:
        self.repo.git("rebase", "-q", "origin/main")
        for mode in ("fail", "garbage"):
            with self.subTest(mode=mode):
                self.assertQuiet(self.repo.push(FAKE_GH_MODE=mode))

    def test_hanging_lookup_is_bounded_and_silent(self) -> None:
        self.repo.git("rebase", "-q", "origin/main")
        import time

        start = time.monotonic()
        res = self.repo.push(FAKE_GH_MODE="hang", PULP_QUEUE_REBASE_TIMEOUT="0.5")
        self.assertLess(time.monotonic() - start, 5.0)
        self.assertQuiet(res)

    def test_no_gh_binary_is_silent(self) -> None:
        self.repo.git("rebase", "-q", "origin/main")
        res = self.repo.push(PULP_QUEUE_REBASE_GH=str(self.repo.tmp / "missing-gh"))
        self.assertQuiet(res)

    def test_override_silences(self) -> None:
        self.repo.git("rebase", "-q", "origin/main")
        self.assertQuiet(self.repo.push(PULP_ALLOW_QUEUE_REBASE="1"))
        self.assertEqual(self.repo.lookups(), 0)

    def test_new_branch_and_delete_records_are_ignored(self) -> None:
        head = self.repo.rev("HEAD")
        for record in (
            f"refs/heads/feature {head} refs/heads/feature {ZERO}\n",
            f"(delete) {ZERO} refs/heads/feature {head}\n",
        ):
            res = subprocess.run(
                [sys.executable, str(SCRIPT), "--root", str(self.repo.dir)],
                input=record, capture_output=True, text=True, env=self.repo.env(), timeout=20,
            )
            self.assertQuiet(res)
        self.assertEqual(self.repo.lookups(), 0)


class QueuedPrGuardHookTests(unittest.TestCase):
    """The hook refuses a push to a queued PR before any gate runs."""

    REFUSAL = "a queued PR does not need a rebase, the queue merges it"

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Repo(Path(self._tmp.name))
        # A content change on top of a rebase: the guard must not care what
        # the push carries, only that the PR is queued.
        self.repo.git("rebase", "-q", "origin/main")
        self.repo.commit("feature.txt", "one\ntwo\n", "more feature work")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def assertProceeded(self, res: subprocess.CompletedProcess) -> None:
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("PULP_SKIP_PREPUSH=1", res.stderr)
        self.assertIn("skipping gates", res.stderr)
        self.assertNotIn(self.REFUSAL, res.stderr)

    def test_queued_pr_refuses_the_push(self) -> None:
        self.repo.set_pr(pr_fixture(queued=True))
        res = self.repo.run_hook(PULP_SKIP_PREPUSH="1")
        self.assertEqual(res.returncode, 1, res.stderr)
        self.assertIn("PR #4242 is in the merge queue", res.stderr)
        self.assertIn(self.REFUSAL, res.stderr)
        self.assertIn("dequeue deliberately first", res.stderr)
        self.assertIn("PULP_ALLOW_QUEUED_PUSH=1", res.stderr)
        self.assertNotIn("skipping gates", res.stderr)

    def test_not_queued_proceeds(self) -> None:
        for fixture in (pr_fixture(), pr_fixture(armed=True), pr_fixture(present=False)):
            with self.subTest(fixture=fixture):
                self.repo.set_pr(fixture)
                self.assertProceeded(self.repo.run_hook(PULP_SKIP_PREPUSH="1"))

    def test_api_error_proceeds_with_a_notice(self) -> None:
        self.repo.set_pr(pr_fixture(queued=True))
        for mode in ("fail", "garbage"):
            with self.subTest(mode=mode):
                res = self.repo.run_hook(PULP_SKIP_PREPUSH="1", FAKE_GH_MODE=mode)
                self.assertProceeded(res)
                self.assertIn("queued-PR check skipped for feature", res.stderr)
                self.assertIn("not blocked", res.stderr)

    def test_hanging_lookup_proceeds_with_a_notice(self) -> None:
        self.repo.set_pr(pr_fixture(queued=True))
        res = self.repo.run_hook(
            PULP_SKIP_PREPUSH="1", FAKE_GH_MODE="hang", PULP_QUEUE_REBASE_TIMEOUT="0.5")
        self.assertProceeded(res)
        self.assertIn("timed out", res.stderr)

    def test_missing_gh_proceeds_with_a_notice(self) -> None:
        self.repo.set_pr(pr_fixture(queued=True))
        res = self.repo.run_hook(
            PULP_SKIP_PREPUSH="1", PULP_QUEUE_REBASE_GH=str(self.repo.tmp / "missing-gh"))
        self.assertProceeded(res)
        self.assertIn("queued-PR check skipped", res.stderr)

    def test_explicit_bypass_proceeds_without_a_lookup(self) -> None:
        self.repo.set_pr(pr_fixture(queued=True))
        res = self.repo.run_hook(
            PULP_SKIP_PREPUSH="1", PULP_ALLOW_QUEUED_PUSH="1", PULP_ALLOW_QUEUE_REBASE="1")
        self.assertProceeded(res)
        self.assertEqual(self.repo.lookups(), 0)

    def test_guard_runs_before_the_diff_cover_build(self) -> None:
        text = HOOK.read_text(encoding="utf-8")
        code = [ln for ln in text.splitlines() if ln.strip() and not ln.lstrip().startswith("#")]
        guard = next(i for i, ln in enumerate(code) if "--refuse-queued" in ln)
        skip = next(i for i, ln in enumerate(code) if 'PULP_SKIP_PREPUSH:-0}" = "1" ]; then' in ln
                    and "DISABLE" not in ln)
        cover = next(i for i, ln in enumerate(code) if 'bash "$DIFF_COVER_SH"' in ln)
        self.assertLess(guard, skip)
        self.assertLess(guard, cover)


if __name__ == "__main__":
    unittest.main()
