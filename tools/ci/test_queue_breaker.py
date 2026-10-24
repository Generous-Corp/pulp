#!/usr/bin/env python3
"""Tests for tools/ci/queue_breaker.py (the merge-queue circuit breaker).

What must hold:
- the state is live only for a well-formed {test, fix_pr, until} whose until
  is in the future and at most MAX_HOURS ahead; unset, malformed, expired and
  too-far states are inactive, and each says why;
- a live breaker FAILS (exit 1, an error annotation, the blocked pull requests
  in the step outputs) a group carrying neither the fix nor a trusted
  queue-unblock label; it never reports a block as a pass;
- the group is admitted when it carries fix_pr, or when queue-unblock was last
  applied by the fleet App (a Bot named shipyard-local) or a repo
  admin/maintainer; a user named shipyard-local, a writer, or a label with no
  trusted applier does not admit;
- the breaker no longer applies once fix_pr has merged;
- it fails open: an unreadable group or label read proceeds;
- the group's pull requests are the first-parent chain of queue merge commits;
- the report-only control says "would fail" and exits 0 (the positive
  control: a synthetic open state that the check demonstrably sees);
- notify comments once per breaker state on each blocked pull request;
- build.yml feeds PULP_QUEUE_BREAKER to the protected base copy on merge
  groups and PULP_QUEUE_BREAKER_CONTROL report-only on pull requests, exports
  the block to a separate notify job that alone holds pull-requests: write and
  checks out only the base branch; and the module imports only the standard
  library, because classify runs that one file on its own.

Run:
    python3 tools/ci/test_queue_breaker.py
"""
from __future__ import annotations

import ast
import contextlib
import io
import json
import os
import pathlib
import sys
import tempfile
import unittest
import urllib.error
from datetime import datetime, timezone

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import queue_breaker as qb  # noqa: E402

NOW = datetime(2026, 10, 23, 2, 0, tzinfo=timezone.utc)
TEST = "gpu-trace-overhead-acceptance-selftest"
REPO = "o/r"


def state(fix=99, until="2026-10-23T04:00:00Z", test=TEST):
    return json.dumps({"test": test, "fix_pr": fix, "until": until})


class FakeGitHub:
    """A merge group of queue merge commits over `base`, plus pull requests,
    label events, collaborator permissions and comments."""

    def __init__(self, prs=(10, 11), pulls=None, labelers=None, permissions=None,
                 comments=None, fail_on=None):
        self.commits = {}
        parent = "base"
        for number in prs:
            sha = f"g{number}"
            self.commits[sha] = {"parents": [{"sha": parent}, {"sha": f"h{number}"}],
                                 "message": f"Merge pull request #{number} from o/b\n\nbody"}
            parent = sha
        self.head = parent
        self.pulls = pulls or {}
        self.labelers = labelers or {}
        self.permissions = permissions or {}
        self.comments = comments or {}
        self.fail_on = fail_on
        self.posted = []

    def __call__(self, method, path, payload=None):
        if self.fail_on and self.fail_on in path:
            raise urllib.error.URLError("unreadable")
        if path.startswith(f"repos/{REPO}/git/commits/"):
            return self.commits[path.rsplit("/", 1)[1]]
        if "/collaborators/" in path:
            login = path.split("/collaborators/")[1].split("/")[0]
            return {"permission": self.permissions.get(login, "read")}
        if path.startswith(f"repos/{REPO}/pulls/"):
            return self.pulls.get(int(path.rsplit("/", 1)[1]), {"labels": []})
        if path == "graphql":
            number = payload["variables"]["number"]
            nodes = [{"label": {"name": qb.UNBLOCK_LABEL}, "actor": actor}
                     for actor in self.labelers.get(number, [])]
            return {"data": {"repository": {"pullRequest": {"timelineItems": {"nodes": nodes}}}}}
        if method == "GET" and "/comments" in path:
            return self.comments.get(int(path.split("/issues/")[1].split("/")[0]), [])
        if method == "POST" and path.endswith("/comments"):
            self.posted.append((int(path.split("/issues/")[1].split("/")[0]), payload["body"]))
            return {}
        raise AssertionError(f"unexpected {method} {path}")


def labelled(number):
    return {number: {"labels": [{"name": qb.UNBLOCK_LABEL}]}}


def run(fake, raw, *args):
    out = io.StringIO()
    with tempfile.NamedTemporaryFile("r", suffix=".out") as outputs:
        old = os.environ.get("QUEUE_BREAKER")
        os.environ["QUEUE_BREAKER"] = raw
        try:
            with contextlib.redirect_stdout(out):
                code = qb.main(["check-group", "--repo", REPO, "--output", outputs.name, *args],
                               fetch=fake, now=NOW)
        finally:
            if old is None:
                os.environ.pop("QUEUE_BREAKER", None)
            else:
                os.environ["QUEUE_BREAKER"] = old
        return code, out.getvalue(), pathlib.Path(outputs.name).read_text(encoding="utf-8")


class State(unittest.TestCase):
    def test_live(self):
        live, note = qb.parse_state(state(), NOW)
        self.assertEqual(note, "open")
        self.assertEqual((live["test"], live["fix_pr"]), (TEST, 99))

    def test_inactive_states_say_why(self):
        cases = {
            "": "unset",
            "not json": "malformed",
            json.dumps({"test": TEST}): "malformed",
            state(test=" "): "malformed",
            state(until="2026-10-23T04:00:00"): "no timezone",
            state(until="2026-10-23T01:59:00Z"): "expired",
            state(until="2026-10-23T05:01:00Z"): "more than 3 h ahead",
        }
        for raw, why in cases.items():
            live, note = qb.parse_state(raw, NOW)
            self.assertIsNone(live, raw)
            self.assertIn(why, note, raw)

    def test_no_fix_is_allowed(self):
        self.assertIsNone(qb.parse_state(state(fix=None), NOW)[0]["fix_pr"])


class Decide(unittest.TestCase):
    def test_blocks_a_group_without_the_fix(self):
        fake = FakeGitHub()
        code, out, outputs = run(fake, state(), "--base", "base", "--head", fake.head)
        self.assertEqual(code, 1)
        self.assertIn("::error title=queue-breaker::queue breaker: block:", out)
        self.assertIn(TEST, out)
        self.assertIn("blocked_prs=11 10", outputs)

    def test_admits_the_fix(self):
        fake = FakeGitHub(prs=(10, 99))
        code, out, outputs = run(fake, state(), "--base", "base", "--head", fake.head)
        self.assertEqual(code, 0)
        self.assertIn("admit: this group carries the fix #99", out)
        self.assertEqual(outputs, "")

    def test_admits_queue_unblock_from_the_fleet_app_or_an_admin(self):
        for actor, permissions in (
                ({"__typename": "Bot", "login": "shipyard-local"}, {}),
                ({"__typename": "User", "login": "owner"}, {"owner": "admin"}),
                ({"__typename": "User", "login": "lead"}, {"lead": "maintain"})):
            fake = FakeGitHub(pulls=labelled(11), labelers={11: [actor]}, permissions=permissions)
            code, out, _ = run(fake, state(), "--base", "base", "--head", fake.head)
            self.assertEqual(code, 0, actor)
            self.assertIn("#11 carries queue-unblock from a trusted actor", out)

    def test_untrusted_queue_unblock_does_not_admit(self):
        untrusted = (
            [{"__typename": "User", "login": "shipyard-local"}],  # a user can take the login
            [{"__typename": "User", "login": "writer"}],
            [{"__typename": "Bot", "login": "dependabot[bot]"}],  # another App
            [{"__typename": "Bot", "login": "shipyard-local"},
             {"__typename": "User", "login": "writer"}],  # the latest applier counts
            [],  # labelled, but no LabeledEvent read back
        )
        for labelers in untrusted:
            fake = FakeGitHub(pulls=labelled(11), labelers={11: labelers},
                              permissions={"writer": "write"})
            code, _, _ = run(fake, state(), "--base", "base", "--head", fake.head)
            self.assertEqual(code, 1, labelers)

    def test_a_label_event_without_the_label_does_not_admit(self):
        fake = FakeGitHub(labelers={11: [{"__typename": "Bot", "login": "shipyard-local"}]})
        self.assertEqual(run(fake, state(), "--base", "base", "--head", fake.head)[0], 1)

    def test_lifts_once_the_fix_merged(self):
        fake = FakeGitHub(pulls={99: {"merged": True, "labels": []}})
        code, out, _ = run(fake, state(), "--base", "base", "--head", fake.head)
        self.assertEqual(code, 0)
        self.assertIn("the fix #99 has merged", out)

    def test_inactive_state_proceeds(self):
        fake = FakeGitHub()
        code, out, _ = run(fake, state(until="2026-10-23T01:00:00Z"), "--base", "base",
                           "--head", fake.head)
        self.assertEqual(code, 0)
        self.assertIn("expired", out)

    def test_fails_open_on_unreadable_input(self):
        for fail_on in ("git/commits", "pulls/", "graphql", "collaborators"):
            fake = FakeGitHub(pulls=labelled(11),
                              labelers={11: [{"__typename": "User", "login": "x"}]},
                              fail_on=fail_on)
            code, out, _ = run(fake, state(), "--base", "base", "--head", fake.head)
            self.assertEqual(code, 0, fail_on)
            self.assertIn("fails open", out, fail_on)

    def test_a_chain_that_misses_the_base_fails_open(self):
        fake = FakeGitHub()
        code, out, _ = run(fake, state(), "--base", "elsewhere", "--head", fake.head)
        self.assertEqual(code, 0)
        self.assertIn("fails open", out)

    def test_group_prs_is_the_first_parent_chain(self):
        fake = FakeGitHub(prs=(10, 11, 12))
        self.assertEqual(qb.group_prs(fake, REPO, "base", fake.head), [12, 11, 10])

    def test_report_only_control_would_fail_and_passes(self):
        code, out, outputs = run(FakeGitHub(), state(fix=None), "--pr", "11", "--report-only")
        self.assertEqual(code, 0)
        self.assertIn("(report-only control) would fail: queue breaker: block:", out)
        self.assertEqual(outputs, "")


class Notify(unittest.TestCase):
    def test_comments_once_per_state(self):
        live, _ = qb.parse_state(state(), NOW)
        fake = FakeGitHub(comments={10: [{"body": "x " + qb.marker(live)}]})
        posted = qb.notify(fake, REPO, state(), NOW, [10, 11], "the reason")
        self.assertEqual(posted, [11])
        self.assertIn("the reason", fake.posted[0][1])
        self.assertIn(qb.marker(live), fake.posted[0][1])

    def test_inactive_state_posts_nothing(self):
        fake = FakeGitHub()
        self.assertEqual(qb.notify(fake, REPO, "", NOW, [10], "r"), [])
        self.assertEqual(fake.posted, [])


def job(build: str, name: str) -> str:
    return build.split(f"\n  {name}:\n", 1)[1].split("\n\n  ", 1)[0]


class Wiring(unittest.TestCase):
    build = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")

    def test_classify_runs_the_protected_copy_with_the_variable(self):
        step = self.build.split("- name: Merge-queue circuit breaker", 1)[1].split("\n      - ", 1)[0]
        self.assertIn("id: queue_breaker", step)
        self.assertIn("vars.PULP_QUEUE_BREAKER ||", step)
        self.assertIn("vars.PULP_QUEUE_BREAKER_CONTROL", step)
        self.assertIn('git show "FETCH_HEAD:tools/ci/queue_breaker.py"', step)
        self.assertIn('--output "$GITHUB_OUTPUT"', step)
        self.assertIn("--report-only", step)
        self.assertNotIn("continue-on-error", step)  # a block must fail the job
        self.assertNotIn("|| true", step)
        classify = job(self.build, "classify")
        self.assertIn("queue_breaker_blocked_prs: ${{ steps.queue_breaker.outputs.blocked_prs }}",
                      classify)
        self.assertNotIn("write", classify.split("    steps:", 1)[0])

    def test_notify_alone_holds_the_write_token(self):
        notify = job(self.build, "queue-breaker-notify")
        self.assertIn("needs: classify", notify)
        self.assertIn("needs.classify.outputs.queue_breaker_blocked_prs != ''", notify)
        self.assertIn("pull-requests: write", notify)
        self.assertIn("ref: ${{ github.event.merge_group.base_ref }}", notify)
        self.assertIn("persist-credentials: false", notify)
        self.assertIn("tools/ci/queue_breaker.py notify", notify)
        self.assertEqual(self.build.count("pull-requests: write"), 1)

    def test_top_level_imports_are_standard_library(self):
        tree = ast.parse((HERE / "queue_breaker.py").read_text(encoding="utf-8"))
        names = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                names |= {alias.name.split(".")[0] for alias in node.names}
            elif isinstance(node, ast.ImportFrom) and node.module:
                names.add(node.module.split(".")[0])
        self.assertTrue(names)
        self.assertEqual(names - set(sys.stdlib_module_names) - {"__future__"}, set())


if __name__ == "__main__":
    unittest.main(verbosity=2)
