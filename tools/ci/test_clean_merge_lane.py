#!/usr/bin/env python3
"""Tests for tools/ci/clean_merge_lane.py (the clean-merge macOS lane).

What must hold:
- `clean-merge` only for a `synchronize` head that is exactly a two-parent
  merge of the previous head and a commit on the base branch's first-parent
  chain, whose tree equals `git merge-tree --write-tree` of those parents, and
  whose previous head's newest trusted `macos` check is a full-lane success;
- every other shape, and every unreadable input, is `full` with a reason;
- no chaining: a previous green that was a contract or clean-merge lane is `full`;
- only this repository's build.yml same-repository pull_request runs count;
- shadow annotates `would_take` and runs the full lane; enforce takes it; off
  decides nothing;
- the git inputs come from real git, so a hand resolution in a merge commit is
  seen (a different tree) on an actual repository.

Run:
    python3 tools/ci/test_clean_merge_lane.py
"""
from __future__ import annotations

import datetime as dt
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import clean_merge_lane as cml  # noqa: E402

REPO = "Generous-Corp/pulp"
BEFORE = "b" * 40
HEAD = "c" * 40
MAIN = "a" * 40
TREE = "d" * 40


def full_lane_check(**overrides):
    check = {
        "check_run_id": 100, "app": cml.ACTIONS_APP, "status": "completed",
        "conclusion": "success",
        "run": {"path": cml.WORKFLOW_PATH, "event": "pull_request", "repository": REPO,
                "head_repository": REPO, "head_sha": BEFORE},
        "steps": {"Build": "success", "Test (non-Windows)": "skipped"},
        "tiers": ["fast"], "lane_verdicts": [],
    }
    check.update(overrides)
    return check


def inputs(**overrides):
    base = {
        "action": "synchronize", "before": BEFORE, "head": HEAD,
        "head_parents": [BEFORE, MAIN], "head_tree": TREE,
        "on_base_chain": {MAIN: True}, "merge_tree": {"exit": 0, "tree": TREE},
        "previous": [full_lane_check()], "repository": REPO,
    }
    base.update(overrides)
    return base


def lane(**overrides):
    return cml.decide(**inputs(**overrides))


class DecideTest(unittest.TestCase):
    def test_a_clean_merge_of_a_full_lane_green_takes_the_lane(self) -> None:
        decision = lane()
        self.assertEqual(decision["lane"], "clean-merge", decision["reason"])
        self.assertEqual(decision["carried_from"], BEFORE)
        # Parent order does not matter: main may be the first parent.
        self.assertEqual(lane(head_parents=[MAIN, BEFORE])["lane"], "clean-merge")
        # A previous head whose full suite ran is also the full lane.
        self.assertEqual(lane(previous=[full_lane_check(tiers=["full"])])["lane"], "clean-merge")

    def test_a_conflict_resolution_in_the_tree_is_full(self) -> None:
        self.assertEqual(lane(merge_tree={"exit": 1, "tree": TREE})["lane"], "full")
        # Clean merge-tree, but the head's tree is not its result: a hand edit.
        decision = lane(merge_tree={"exit": 0, "tree": "e" * 40})
        self.assertEqual(decision["lane"], "full")
        self.assertIn("tree differs", decision["reason"])
        self.assertEqual(lane(head_tree=None)["lane"], "full")

    def test_any_other_parent_set_is_full(self) -> None:
        for parents in ([BEFORE], [BEFORE, MAIN, "e" * 40], ["e" * 40, MAIN], [BEFORE, BEFORE], []):
            with self.subTest(parents=parents):
                self.assertEqual(lane(head_parents=parents)["lane"], "full")
        self.assertEqual(lane(head_parents=None)["lane"], "full")

    def test_the_merged_parent_must_be_on_mains_first_parent_chain(self) -> None:
        self.assertEqual(lane(on_base_chain={MAIN: False})["lane"], "full")
        self.assertEqual(lane(on_base_chain=None)["lane"], "full")
        self.assertEqual(lane(on_base_chain={})["lane"], "full")

    def test_only_a_synchronize_with_a_real_before_qualifies(self) -> None:
        for action in ("opened", "reopened", "labeled", ""):
            with self.subTest(action=action):
                self.assertEqual(lane(action=action)["lane"], "full")
        for before in ("", "0" * 40, "short", None):
            with self.subTest(before=before):
                self.assertEqual(lane(before=before)["lane"], "full")
        self.assertEqual(lane(head="")["lane"], "full")

    def test_no_chaining_from_a_contract_or_clean_merge_green(self) -> None:
        contract = full_lane_check(steps={"Build": "skipped"}, tiers=["deferred"])
        self.assertEqual(lane(previous=[contract])["lane"], "full")
        took = full_lane_check(steps={"Build": "skipped"}, tiers=["clean-merge"],
                               lane_verdicts=["took"])
        self.assertEqual(lane(previous=[took])["lane"], "full")
        # Even with a full-lane tier, a `took` note means the build was skipped.
        self.assertEqual(lane(previous=[full_lane_check(lane_verdicts=["took"])])["lane"], "full")
        # A shadow-mode `would_take` head ran the full lane, so it counts.
        self.assertEqual(lane(previous=[full_lane_check(lane_verdicts=["would_take"])])["lane"],
                         "clean-merge")

    def test_a_previous_green_must_have_built_and_carry_a_full_lane_tier(self) -> None:
        for check in (full_lane_check(steps={"Build": "skipped"}),
                      full_lane_check(steps={}),
                      full_lane_check(steps=None),
                      full_lane_check(tiers=[]),
                      full_lane_check(tiers=["not-required"]),
                      full_lane_check(tiers=["fast", "deferred"]),
                      full_lane_check(tiers=["receipt-reused"])):
            with self.subTest(check=check):
                self.assertEqual(lane(previous=[check])["lane"], "full")

    def test_only_this_repositorys_build_yml_pull_request_runs_count(self) -> None:
        def run(**over):
            r = dict(full_lane_check()["run"])
            r.update(over)
            return full_lane_check(run=r)
        for check in (run(head_repository="someone/fork"),
                      run(repository="someone/pulp"),
                      run(path=".github/workflows/sanitizers.yml"),
                      run(event="workflow_dispatch"),
                      run(event="merge_group"),
                      run(head_sha="e" * 40),
                      full_lane_check(app="some-other-app")):
            with self.subTest(check=check):
                self.assertEqual(lane(previous=[check])["lane"], "full")

    def test_unreadable_checks_are_full(self) -> None:
        self.assertEqual(lane(previous=None)["lane"], "full")
        self.assertEqual(lane(previous=[])["lane"], "full")
        self.assertEqual(lane(previous=[full_lane_check(), full_lane_check(check_run_id=101, run=None)])["lane"],
                         "full")

    def test_the_newest_trusted_check_decides(self) -> None:
        older_green = full_lane_check(check_run_id=100)
        newer_red = full_lane_check(check_run_id=200, conclusion="failure")
        self.assertEqual(lane(previous=[older_green, newer_red])["lane"], "full")
        running = full_lane_check(check_run_id=200, status="in_progress", conclusion=None)
        self.assertEqual(lane(previous=[older_green, running])["lane"], "full")
        # Control: the newer green wins over an older red.
        older_red = full_lane_check(check_run_id=100, conclusion="failure")
        self.assertEqual(lane(previous=[older_red, full_lane_check(check_run_id=200)])["lane"],
                         "clean-merge")
        # Another app's `macos` check is not this repository's.
        other_app = full_lane_check(check_run_id=300, app="other-app", conclusion="failure")
        self.assertEqual(lane(previous=[older_green, other_app])["lane"], "clean-merge")

    def test_a_newer_red_from_another_event_or_workflow_is_not_passed_over(self) -> None:
        older_green = full_lane_check(check_run_id=100)
        for run in ({"event": "workflow_dispatch"}, {"path": ".github/workflows/build-macos.yml"},
                    {"head_repository": "someone/fork"}):
            with self.subTest(run=run):
                newer = full_lane_check(check_run_id=200, conclusion="failure",
                                        run={**full_lane_check()["run"], **run})
                self.assertEqual(lane(previous=[older_green, newer])["lane"], "full")
                # Even green, a newer non-pull_request check is not the carried lane.
                newer_green = full_lane_check(check_run_id=200, run={**full_lane_check()["run"], **run})
                self.assertEqual(lane(previous=[older_green, newer_green])["lane"], "full")


class ModeTest(unittest.TestCase):
    def test_verdicts(self) -> None:
        clean, full = {"lane": "clean-merge"}, {"lane": "full"}
        self.assertEqual(cml.verdict_for("shadow", clean), "would_take")
        self.assertEqual(cml.verdict_for("enforce", clean), "took")
        self.assertEqual(cml.verdict_for("enforce", full), "full")
        self.assertEqual(cml.verdict_for("shadow", full), "full")

    def test_note_is_a_notice_with_the_schema(self) -> None:
        line = cml.note("shadow", "would_take", lane(), HEAD, BEFORE)
        prefix = f"::notice title={cml.NOTE_TITLE}::"
        self.assertTrue(line.startswith(prefix))
        record = json.loads(line[len(prefix):])
        self.assertEqual(record["schema"], "pulp-clean-merge-lane/v1")
        self.assertEqual(record["verdict"], "would_take")
        self.assertEqual(record["carried_from"], BEFORE)

    def _decide(self, mode, decision):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            args = cml.argparse.Namespace(
                repo=tmp, repository=REPO, token="t", head=HEAD, before=BEFORE,
                base_ref="main", action="synchronize", mode=mode, github_output=str(out))
            with mock.patch.object(cml, "gather_git", return_value={
                    "head_parents": [BEFORE, MAIN], "head_tree": TREE,
                    "on_base_chain": {MAIN: True}, "merge_tree": {"exit": 0, "tree": TREE}}), \
                    mock.patch.object(cml, "gather_previous",
                                      return_value=[full_lane_check()] if decision else []), \
                    mock.patch.dict(os.environ, {"GITHUB_STEP_SUMMARY": ""}), \
                    mock.patch("sys.stdout") as stdout:
                self.assertEqual(cml.cmd_decide(args), 0)
            printed = "".join(c.args[0] for c in stdout.write.call_args_list)
            values = dict(line.split("=", 1) for line in out.read_text(encoding="utf-8").splitlines())
            return values, printed

    def test_shadow_annotates_and_runs_the_full_lane(self) -> None:
        values, printed = self._decide("shadow", True)
        self.assertEqual(values, {"decision": "clean-merge", "lane": "full",
                                  "carried_from": BEFORE, "mode": "shadow"})
        self.assertIn('"verdict": "would_take"', printed)

    def test_enforce_takes_the_lane(self) -> None:
        values, printed = self._decide("enforce", True)
        self.assertEqual(values["lane"], "clean-merge")
        self.assertIn('"verdict": "took"', printed)
        values, _ = self._decide("enforce", False)
        self.assertEqual(values["lane"], "full")

    def test_off_and_unknown_modes(self) -> None:
        values, printed = self._decide("off", True)
        self.assertEqual(values["lane"], "full")
        self.assertEqual(values["decision"], "full")
        self.assertNotIn(cml.NOTE_TITLE, printed)
        values, _ = self._decide("bogus", True)
        self.assertEqual(values["mode"], "shadow")
        self.assertEqual(values["lane"], "full")

    def test_a_crashing_gather_is_full(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            args = cml.argparse.Namespace(
                repo=tmp, repository=REPO, token="t", head=HEAD, before=BEFORE,
                base_ref="main", action="synchronize", mode="enforce", github_output=str(out))
            with mock.patch.object(cml, "gather_git", side_effect=OSError("boom")), \
                    mock.patch("sys.stdout"):
                self.assertEqual(cml.cmd_decide(args), 0)
            self.assertIn("lane=full", out.read_text(encoding="utf-8"))

    def test_an_api_failure_is_full(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            args = cml.argparse.Namespace(
                repo=tmp, repository=REPO, token="t", head=HEAD, before=BEFORE,
                base_ref="main", action="synchronize", mode="enforce", github_output=str(out))
            with mock.patch.object(cml, "gather_git", return_value={
                    "head_parents": [BEFORE, MAIN], "head_tree": TREE,
                    "on_base_chain": {MAIN: True}, "merge_tree": {"exit": 0, "tree": TREE}}), \
                    mock.patch.object(cml, "gather_previous", side_effect=OSError("HTTP 502")), \
                    mock.patch("sys.stdout"):
                cml.cmd_decide(args)
            self.assertIn("lane=full", out.read_text(encoding="utf-8"))


def _git(repo, *args, check=True):
    env = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.com",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.com",
           "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}
    return subprocess.run(["git", *args], cwd=repo, check=check, capture_output=True,
                          text=True, encoding="utf-8", env=env).stdout.strip()


class RealGitTest(unittest.TestCase):
    """gather_git on an actual repository: origin/main, a pull-request branch,
    and merges of main into it."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.origin = root / "origin"
        self.origin.mkdir()
        _git(self.origin, "init", "-q", "-b", "main")
        (self.origin / "a.txt").write_text("one\ntwo\nthree\n", encoding="utf-8")
        (self.origin / "b.txt").write_text("b\n", encoding="utf-8")
        _git(self.origin, "add", ".")
        _git(self.origin, "commit", "-q", "-m", "base")
        self.work = root / "work"
        _git(root, "clone", "-q", str(self.origin), str(self.work))
        _git(self.work, "checkout", "-q", "-b", "feature")
        (self.work / "a.txt").write_text("one\ntwo\nthree\nfeature\n", encoding="utf-8")
        _git(self.work, "commit", "-q", "-am", "feature")
        self.before = _git(self.work, "rev-parse", "HEAD")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _advance_main(self, path="b.txt", text="b\nmain\n"):
        (self.origin / path).write_text(text, encoding="utf-8")
        _git(self.origin, "commit", "-q", "-am", "main moves")
        _git(self.work, "fetch", "-q", "origin")

    def _decide(self, head):
        git_inputs = cml.gather_git(self.work, head, self.before, "main")
        return cml.decide(action="synchronize", before=self.before, head=head,
                          previous=[full_lane_check(run={**full_lane_check()["run"],
                                                         "head_sha": self.before})],
                          repository=REPO, **git_inputs)

    def test_a_clean_merge_of_main(self) -> None:
        self._advance_main()
        _git(self.work, "merge", "-q", "--no-edit", "origin/main")
        decision = self._decide(_git(self.work, "rev-parse", "HEAD"))
        self.assertEqual(decision["lane"], "clean-merge", decision["reason"])

    def test_a_merge_with_a_hand_edit_is_full(self) -> None:
        self._advance_main()
        _git(self.work, "merge", "-q", "--no-commit", "--no-ff", "origin/main")
        (self.work / "b.txt").write_text("b\nmain\nsneaked in\n", encoding="utf-8")
        _git(self.work, "add", "b.txt")
        _git(self.work, "commit", "-q", "--no-edit")
        decision = self._decide(_git(self.work, "rev-parse", "HEAD"))
        self.assertEqual(decision["lane"], "full")
        self.assertIn("tree differs", decision["reason"])

    def test_a_resolved_conflict_is_full(self) -> None:
        self._advance_main("a.txt", "one\ntwo\nthree\nmain\n")
        # Conflicts by construction; run under the same identity/config as every
        # other step so a runner without a global git identity behaves the same.
        _git(self.work, "merge", "-q", "--no-commit", "origin/main", check=False)
        (self.work / "a.txt").write_text("one\ntwo\nthree\nfeature\nmain\n", encoding="utf-8")
        _git(self.work, "add", "a.txt")
        _git(self.work, "commit", "-q", "--no-edit")
        decision = self._decide(_git(self.work, "rev-parse", "HEAD"))
        self.assertEqual(decision["lane"], "full")
        self.assertIn("conflict", decision["reason"])

    def test_a_merge_of_a_side_branch_is_full(self) -> None:
        # The merged parent is not on main's first-parent chain.
        _git(self.origin, "checkout", "-q", "-b", "side")
        (self.origin / "c.txt").write_text("side\n", encoding="utf-8")
        _git(self.origin, "add", "c.txt")
        _git(self.origin, "commit", "-q", "-m", "side")
        _git(self.origin, "checkout", "-q", "main")
        _git(self.work, "fetch", "-q", "origin")
        _git(self.work, "merge", "-q", "--no-edit", "origin/side")
        decision = self._decide(_git(self.work, "rev-parse", "HEAD"))
        self.assertEqual(decision["lane"], "full")
        self.assertIn("first-parent chain", decision["reason"])

    def test_an_ordinary_commit_is_full(self) -> None:
        (self.work / "a.txt").write_text("changed\n", encoding="utf-8")
        _git(self.work, "commit", "-q", "-am", "edit")
        decision = self._decide(_git(self.work, "rev-parse", "HEAD"))
        self.assertEqual(decision["lane"], "full")
        self.assertIn("1 parents", decision["reason"])

    def test_a_shallow_clone_reads_nothing(self) -> None:
        self._advance_main()
        _git(self.work, "merge", "-q", "--no-edit", "origin/main")
        head = _git(self.work, "rev-parse", "HEAD")
        shallow = Path(self.tmp.name) / "shallow"
        _git(Path(self.tmp.name), "clone", "-q", "--depth", "1",
             f"file://{self.work}", str(shallow))
        git_inputs = cml.gather_git(shallow, head, self.before, "main")
        self.assertIsNone(git_inputs["head_parents"])


class GatherPreviousTest(unittest.TestCase):
    def test_reads_runs_jobs_and_annotations_of_actions_checks_only(self) -> None:
        calls = []

        def fetch(url, token):
            calls.append(url)
            if "/check-runs?" in url:
                return {"check_runs": [
                    {"id": 11, "name": "macos", "status": "completed", "conclusion": "success",
                     "app": {"slug": "github-actions"},
                     "details_url": f"https://github.com/{REPO}/actions/runs/77/job/88"},
                    {"id": 12, "name": "macos", "status": "completed", "conclusion": "success",
                     "app": {"slug": "someone-else"}, "details_url": "https://example.com/x"},
                ]}
            if url.endswith("/actions/runs/77"):
                return {"path": ".github/workflows/build.yml", "event": "pull_request",
                        "repository": {"full_name": REPO}, "head_repository": {"full_name": REPO},
                        "head_sha": BEFORE}
            if url.endswith("/actions/jobs/88"):
                return {"steps": [{"name": "Build", "conclusion": "success"}]}
            if "/check-runs/11/annotations" in url:
                return [{"title": "shipyard-test-tier", "message": '{"tier":"fast"}'},
                        {"title": "pulp-clean-merge-lane", "message": '{"verdict":"would_take"}'},
                        {"title": "other", "message": "x"}]
            raise AssertionError(url)

        checks = cml.gather_previous(REPO, "t", BEFORE, fetch=fetch)
        self.assertEqual(len(checks), 2)
        first = checks[0]
        self.assertEqual(first["tiers"], ["fast"])
        self.assertEqual(first["lane_verdicts"], ["would_take"])
        self.assertEqual(first["steps"], {"Build": "success"})
        self.assertEqual(first["run"]["head_sha"], BEFORE)
        self.assertIsNone(checks[1]["run"], "a foreign app's check is not followed")
        self.assertEqual(lane(previous=checks)["lane"], "clean-merge")


class ReportTest(unittest.TestCase):
    def _fetch(self, listed=None):
        seen = []

        def fetch(url, token):
            seen.append(url)
            if "/workflows/build.yml/runs" in url:
                if "created=2026-11-01" in url:
                    runs = [{"id": 1, "head_sha": "1" * 40}, {"id": 2, "head_sha": "2" * 40}]
                    return {"total_count": listed or 2, "workflow_runs": runs}
                return {"total_count": 0, "workflow_runs": []}
            if "/runs/1/jobs" in url:
                return {"jobs": [{"name": "macos", "check_run_url": "https://x/check-runs/10",
                                  "steps": [{"name": "Build", "conclusion": "success"}]}]}
            if "/runs/2/jobs" in url:
                return {"jobs": [
                    {"name": "macos", "check_run_url": "https://x/check-runs/20",
                     "steps": [{"name": "Build", "conclusion": "success"},
                               {"name": "Test (non-Windows)", "conclusion": "failure"}]},
                    {"name": "linux", "check_run_url": "https://x/check-runs/21"}]}
            if "/check-runs/10/annotations" in url or "/check-runs/20/annotations" in url:
                return [{"title": "pulp-clean-merge-lane", "message": '{"verdict":"would_take"}'}]
            raise AssertionError(url)
        return fetch, seen

    def test_counts_the_window_and_flags_a_would_take_head_that_failed(self) -> None:
        fetch, seen = self._fetch()
        result = cml.report(REPO, "t", "2026-11-01", fetch=fetch, today=dt.date(2026, 11, 9))
        self.assertEqual(result["would_take"], 2)
        self.assertEqual(result["would_take_heads"], 2)
        self.assertEqual(result["days"], 8)
        self.assertEqual(len(result["would_take_failed"]), 1)
        self.assertEqual(result["would_take_failed"][0]["failed_steps"], ["Test (non-Windows)"])
        self.assertFalse(result["ready_to_enforce"])
        # One listing per day, every attempt's jobs.
        self.assertEqual(sum("/workflows/build.yml/runs" in u for u in seen), 9)
        self.assertTrue(all("filter=all" in u for u in seen if "/jobs" in u))

    def test_a_rerun_counts_its_head_once(self) -> None:
        def fetch(url, token):
            if "/workflows/build.yml/runs" in url:
                runs = [{"id": 1, "head_sha": "1" * 40}] if "created=2026-11-09" in url else []
                return {"total_count": len(runs), "workflow_runs": runs}
            if "/runs/1/jobs" in url:
                job = {"name": "macos", "steps": [{"name": "Build", "conclusion": "success"}]}
                return {"jobs": [{**job, "check_run_url": "https://x/check-runs/10"},
                                 {**job, "check_run_url": "https://x/check-runs/11"}]}
            if "/annotations" in url:
                return [{"title": "pulp-clean-merge-lane", "message": '{"verdict":"would_take"}'}]
            raise AssertionError(url)
        result = cml.report(REPO, "t", "2026-11-09", fetch=fetch, today=dt.date(2026, 11, 9))
        self.assertEqual(result["would_take"], 2)
        self.assertEqual(result["would_take_heads"], 1)

    def test_a_truncated_listing_is_refused(self) -> None:
        fetch, _ = self._fetch(listed=1500)
        with self.assertRaises(RuntimeError):
            cml.report(REPO, "t", "2026-11-01", fetch=fetch, today=dt.date(2026, 11, 9))

    def test_annotations_are_paginated(self) -> None:
        pages = {1: [{"title": "x", "message": ""}] * 100,
                 2: [{"title": "shipyard-test-tier", "message": '{"tier":"fast"}'}]}

        def fetch(url, token):
            return pages.get(int(url.rsplit("page=", 1)[1]), [])
        items = cml._annotations(REPO, "t", 5, fetch)
        self.assertEqual(len(items), 101)

if __name__ == "__main__":
    unittest.main()
