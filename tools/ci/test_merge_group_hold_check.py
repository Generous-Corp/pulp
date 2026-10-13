#!/usr/bin/env python3
"""Tests for tools/ci/merge_group_hold_check.py (the merge-group HOLD backstop).

What must hold:
- the group's pull requests are exactly the first-parent chain of queue merge
  commits from the group head to merge_group.base_sha, each landing its second
  parent; a pull request's own merge commits are not entries;
- a HOLD as the latest trusted verdict on a queued head fails with the pull
  request, the HOLD's URL and the fix; an APPROVE after it unblocks;
- trust is Shipyard's rule: the fleet App or OWNER/MEMBER/COLLABORATOR, never
  the distinguishable author; a tie goes to the HOLD;
- on Shipyard's captured answer for #10114 (fixtures/merge_group_hold/, the
  same file Shipyard's scorecard tests read) the verdict is HOLD;
- anything unreadable passes with a warning, never fails the queue;
- build.yml runs the protected base branch's copy in the merge_group
  preamble, handing the token to that copy only.

Run:
    python3 tools/ci/test_merge_group_hold_check.py
"""
from __future__ import annotations

import contextlib
import io
import json
import pathlib
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import merge_group_hold_check as hold  # noqa: E402

ROOT = HERE.parents[1]
BASE = "0" * 40
PR_A = "a" * 40
PR_B = "b" * 40
MERGE_A = "1" * 40
MERGE_B = "2" * 40
INNER = "3" * 40


def commit(sha: str, parents: list, subject: str) -> dict:
    return {"sha": sha, "parents": [{"sha": p} for p in parents],
            "commit": {"message": f"{subject}\n\nbody"}}


def compare() -> dict:
    # Group: base <- merge #11 (lands A) <- merge #12 (lands B). B's own
    # history carries a `Merge pull request #5` commit that is not an entry.
    return {"commits": [
        commit(INNER, [BASE, PR_A], "Merge pull request #5 from o/inner"),
        commit(PR_B, [INNER], "work on B"),
        commit(PR_A, [BASE], "work on A"),
        commit(MERGE_A, [BASE, PR_A], "Merge pull request #11 from o/a"),
        commit(MERGE_B, [MERGE_A, PR_B], "Merge pull request #12 from o/b"),
    ]}


def node(login: str, association: str, at: str, body: str, url: str = "u") -> dict:
    return {"author": {"login": login}, "authorAssociation": association,
            "createdAt": at, "body": body, "url": url}


def pr(comments: list, reviews: list = (), author: str = "alice") -> dict:
    return {"headRefOid": PR_B, "author": {"login": author},
            "comments": {"totalCount": len(comments), "nodes": list(comments)},
            "reviews": {"totalCount": len(reviews), "nodes": list(reviews)}}


class Fake:
    def __init__(self, verdicts: dict, compare_answer=None):
        self.verdicts = verdicts
        self.compare_answer = compare() if compare_answer is None else compare_answer
        self.calls = []

    def __call__(self, method, path, payload=None):
        self.calls.append((method, path))
        if path.startswith("repos/o/r/compare/"):
            if isinstance(self.compare_answer, Exception):
                raise self.compare_answer
            return self.compare_answer
        number = payload["variables"]["number"]
        value = self.verdicts[number]
        if isinstance(value, Exception):
            raise value
        return {"data": {"repository": {"pullRequest": value}}}


def run(fake: Fake) -> tuple:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = hold.main(["--repo", "o/r", "--base", BASE, "--head", MERGE_B], fetch=fake)
    return code, out.getvalue()


class MergeGroupHoldCheck(unittest.TestCase):
    def test_the_entries_are_the_first_parent_chain_of_queue_merges(self) -> None:
        entries = hold.group_entries(Fake({}), "o/r", BASE, MERGE_B)
        self.assertEqual(entries, [(12, PR_B), (11, PR_A)])

    def test_a_hold_on_a_queued_head_fails_and_says_what_to_do(self) -> None:
        held = pr([node("shipyard-local", "CONTRIBUTOR", "2026-10-12T10:00:00Z",
                        f"Delta reviewed.\n\nverdict: HOLD head:{PR_B}", url="https://x/hold")])
        code, out = run(Fake({12: held, 11: pr([])}))
        self.assertEqual(code, 1, out)
        self.assertIn("::error title=merge-group HOLD check::#12 is held", out)
        self.assertIn("https://x/hold", out)
        self.assertIn("Fix: push a new head", out)
        self.assertIn("https://github.com/o/r/pull/12", out)
        self.assertNotIn("#11 is held", out)

    def test_an_approve_after_the_hold_unblocks(self) -> None:
        cleared = pr([
            node("shipyard-local", "CONTRIBUTOR", "2026-10-12T10:00:00Z", f"verdict: HOLD head:{PR_B}"),
            node("daniel", "OWNER", "2026-10-12T11:00:00Z", f"verdict: APPROVE head:{PR_B}"),
        ])
        code, out = run(Fake({12: cleared, 11: pr([])}))
        self.assertEqual(code, 0, out)
        self.assertIn("no queued pull request is held", out)

    def test_only_a_trusted_verdict_on_that_head_counts(self) -> None:
        hold_b = f"verdict: HOLD head:{PR_B}"
        for comments, author in [
            ([node("drive-by", "NONE", "2026-10-12T10:00:00Z", hold_b)], "alice"),
            ([node("alice", "OWNER", "2026-10-12T10:00:00Z", hold_b)], "alice"),
            ([node("daniel", "OWNER", "2026-10-12T10:00:00Z", f"verdict: HOLD head:{PR_A}")], "alice"),
            ([node("daniel", "OWNER", "2026-10-12T10:00:00Z", f"> verdict: HOLD head:{PR_B}")], "alice"),
        ]:
            self.assertIsNone(hold.latest_verdict(pr(comments, author=author), PR_B), comments)
        # The fleet App counts on its own pull request; a review decides too.
        self.assertFalse(hold.latest_verdict(
            pr([node("shipyard-local[bot]", "NONE", "2026-10-12T10:00:00Z", hold_b)],
               author="shipyard-local"), PR_B)["approve"])
        review = {"author": {"login": "daniel"}, "authorAssociation": "MEMBER",
                  "state": "CHANGES_REQUESTED", "submittedAt": "2026-10-12T10:00:00Z",
                  "url": "r", "commit": {"oid": PR_B}}
        self.assertFalse(hold.latest_verdict(pr([], [review]), PR_B)["approve"])
        # A tie goes to the HOLD.
        tie = pr([node("daniel", "OWNER", "2026-10-12T10:00:00Z", f"verdict: APPROVE head:{PR_B}"),
                  node("shipyard-local", "NONE", "2026-10-12T10:00:00Z", hold_b)])
        self.assertFalse(hold.latest_verdict(tie, PR_B)["approve"])

    def test_shipyards_captured_10114_reads_hold(self) -> None:
        answer = json.loads((HERE / "fixtures/merge_group_hold/pr-10114-verdicts.json")
                            .read_text(encoding="utf-8"))
        pull = answer["data"]["repository"]["pullRequest"]
        verdict = hold.latest_verdict(pull, pull["headRefOid"])
        self.assertIsNotNone(verdict)
        self.assertFalse(verdict["approve"])

    def test_anything_unreadable_passes_with_a_warning(self) -> None:
        code, out = run(Fake({}, compare_answer=OSError("HTTP 502")))
        self.assertEqual(code, 0)
        self.assertIn("::warning title=merge-group HOLD check::the merge group's pull "
                      "requests could not be read", out)
        broken = {"commits": [commit(MERGE_B, [BASE, PR_B], "Squashed: something")]}
        code, out = run(Fake({}, compare_answer=broken))
        self.assertEqual(code, 0)
        self.assertIn("not a queue merge commit", out)
        code, out = run(Fake({12: OSError("HTTP 403"), 11: pr([])}))
        self.assertEqual(code, 0)
        self.assertIn("#12: its verdicts could not be read", out)

    def test_build_yml_runs_it_in_the_merge_group_preamble(self) -> None:
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        classify = workflow[workflow.index("\n  classify:\n"):workflow.index("\n  protected-receipt-reuse:\n")]
        step = classify[classify.index("- name: Refuse a merge group whose pull request is held"):
                        classify.index("- id: classify")]
        self.assertIn("if: ${{ github.event_name == 'merge_group' }}", step)
        self.assertIn("github.event.merge_group.base_sha", step)
        self.assertNotIn("continue-on-error", step)
        # The token reaches only the protected base branch's copy of the
        # check, never the queue entry's checkout.
        self.assertIn('git show "FETCH_HEAD:tools/ci/merge_group_hold_check.py"', step)
        self.assertIn('GH_TOKEN="${{ github.token }}" python3 "$hold_check"', step)
        self.assertNotIn("GH_TOKEN: ${{ github.token }}", step)
        self.assertNotIn("python3 tools/ci/merge_group_hold_check.py", step)


if __name__ == "__main__":
    unittest.main()
