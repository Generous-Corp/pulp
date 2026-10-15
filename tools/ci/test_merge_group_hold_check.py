#!/usr/bin/env python3
"""Tests for tools/ci/merge_group_hold_check.py (the merge-group HOLD backstop).

What must hold:
- the group's pull requests are exactly the first-parent chain of queue merge
  commits from the group head to merge_group.base_sha, each landing its second
  parent; a pull request's own merge commits are not entries;
- a HOLD as the latest trusted verdict on a queued head fails with the pull
  request, the HOLD's URL and the fix; the fleet App's APPROVE after it
  unblocks;
- an APPROVE counts only from the fleet App: a human account's APPROVE (a
  comment or a review, any association) after the App's HOLD leaves it held;
- a HOLD counts from the fleet App, an OWNER/MEMBER/COLLABORATOR, or the
  author; a tie goes to the HOLD;
- on Shipyard's captured answer for #10114 (fixtures/merge_group_hold/, the
  same file Shipyard's scorecard tests read) the verdict is HOLD;
- it fails closed: a failed read, a GraphQL `errors` answer, a missing token,
  a chain that does not reach the base, or a comment/review list not read to
  its totalCount fails the group, naming the pull request or group, the error
  and the fix;
- comments and reviews are paginated, and the chain is read one commit at a
  time, so a long pull request or a large group is read in full;
- build.yml runs the protected base branch's copy in the merge_group
  preamble, handing the token to that copy only; a failed base fetch fails,
  and only a base without the check passes, with a notice.

Run:
    python3 tools/ci/test_merge_group_hold_check.py
"""
from __future__ import annotations

import contextlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
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
    # The shape of `GET repos/{repo}/git/commits/{sha}`.
    return {"sha": sha, "parents": [{"sha": p} for p in parents],
            "message": f"{subject}\n\nbody"}


def chain() -> dict:
    # Group: base <- merge #11 (lands A) <- merge #12 (lands B). B's own
    # history carries a `Merge pull request #5` commit that is not an entry.
    return {c["sha"]: c for c in [
        commit(INNER, [BASE, PR_A], "Merge pull request #5 from o/inner"),
        commit(PR_B, [INNER], "work on B"),
        commit(PR_A, [BASE], "work on A"),
        commit(MERGE_A, [BASE, PR_A], "Merge pull request #11 from o/a"),
        commit(MERGE_B, [MERGE_A, PR_B], "Merge pull request #12 from o/b"),
    ]}


def node(login: str, association: str, at: str, body: str, url: str = "u",
         kind: str = "") -> dict:
    """A comment as GraphQL answers it: the fleet App is a `Bot`, anyone else
    a `User` unless `kind` says otherwise."""
    kind = kind or ("Bot" if hold.normalized(login) == hold.FLEET_APP else "User")
    return {"author": {"__typename": kind, "login": login}, "authorAssociation": association,
            "createdAt": at, "body": body, "url": url}


def pr(comments: list, reviews: list = (), author: str = "alice") -> dict:
    return {"author": author, "comments": list(comments), "reviews": list(reviews)}


class Fake:
    """Answers the git-commit reads and the paginated GraphQL connections.

    `verdicts[number]` is a `pr(...)`, an exception to raise, or a raw GraphQL
    answer (a dict with `errors`). `lie_total` reports that many items while
    ending the pagination early."""

    def __init__(self, verdicts: dict, commits=None, page: int = 100, lie_total=None):
        self.verdicts = verdicts
        self.commits = chain() if commits is None else commits
        self.page = page
        self.lie_total = lie_total
        self.calls = []

    def __call__(self, method, path, payload=None):
        self.calls.append((method, path))
        if path.startswith("repos/o/r/git/commits/"):
            if isinstance(self.commits, Exception):
                raise self.commits
            return self.commits[path.rsplit("/", 1)[1]]
        variables = payload["variables"]
        value = self.verdicts[variables["number"]]
        if isinstance(value, Exception):
            raise value
        if "errors" in value:
            return value
        field = "comments" if "comments(first:" in payload["query"] else "reviews"
        items = value[field]
        start = int(variables["after"] or 0)
        end = start + self.page
        if self.lie_total is not None:
            end = min(end, self.page)
        page = {"totalCount": self.lie_total or len(items),
                "pageInfo": {"hasNextPage": end < len(items) and self.lie_total is None,
                             "endCursor": str(end)},
                "nodes": items[start:end]}
        return {"data": {"repository": {"pullRequest": {
            "author": {"login": value["author"]}, field: page}}}}


def run(fake: Fake, head: str = MERGE_B) -> tuple:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = hold.main(["--repo", "o/r", "--base", BASE, "--head", head], fetch=fake)
    return code, out.getvalue()


def verdicts_of(value: dict) -> dict:
    """The connection shape `latest_verdict` reads, from a `pr(...)`."""
    author = value["author"]
    return {"author": {"login": author},
            "comments": {"nodes": value["comments"]}, "reviews": {"nodes": value["reviews"]}}


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

    def test_the_fleet_apps_approve_after_the_hold_unblocks(self) -> None:
        cleared = pr([
            node("shipyard-local", "CONTRIBUTOR", "2026-10-12T10:00:00Z", f"verdict: HOLD head:{PR_B}"),
            node("shipyard-local[bot]", "CONTRIBUTOR", "2026-10-12T11:00:00Z",
                 f"verdict: APPROVE head:{PR_B}"),
        ])
        code, out = run(Fake({12: cleared, 11: pr([])}))
        self.assertEqual(code, 0, out)
        self.assertIn("no queued pull request is held", out)

    def test_a_user_account_named_like_the_app_cannot_clear_a_hold(self) -> None:
        # GraphQL names the App `shipyard-local`, a login anyone can register
        # as a user; only the Bot is the App.
        hold_b = node("shipyard-local", "CONTRIBUTOR", "2026-10-12T10:00:00Z",
                      f"verdict: HOLD head:{PR_B}", url="https://x/hold")
        spoof = node("shipyard-local", "NONE", "2026-10-12T11:00:00Z",
                     f"verdict: APPROVE head:{PR_B}", kind="User")
        code, out = run(Fake({12: pr([hold_b, spoof]), 11: pr([])}))
        self.assertEqual(code, 1, out)
        self.assertIn("#12 is held", out)
        app = node("shipyard-local", "CONTRIBUTOR", "2026-10-12T11:00:00Z",
                   f"verdict: APPROVE head:{PR_B}")
        self.assertEqual(run(Fake({12: pr([hold_b, app]), 11: pr([])}))[0], 0,
                         "control: the App's own APPROVE clears it")
        review = {"author": {"__typename": "User", "login": "shipyard-local"},
                  "authorAssociation": "NONE", "state": "APPROVED",
                  "submittedAt": "2026-10-12T11:00:00Z", "url": "r", "commit": {"oid": PR_B}}
        self.assertFalse(hold.latest_verdict(verdicts_of(pr([hold_b], [review])), PR_B)["approve"])
        self.assertIn("author{__typename login}", hold.COMMENTS_QUERY)
        self.assertIn("author{__typename login}", hold.REVIEWS_QUERY)

    def test_a_human_approve_after_the_apps_hold_leaves_it_held(self) -> None:
        # pulp#10293/#10302: the owner's personal login approved pull requests an
        # agent then armed. Posted after the App's HOLD, it must not clear it.
        hold_b = node("shipyard-local", "CONTRIBUTOR", "2026-10-12T10:00:00Z",
                      f"verdict: HOLD head:{PR_B}", url="https://x/hold")
        comment = node("danielraffel", "OWNER", "2026-10-12T11:00:00Z",
                       f"verdict: APPROVE head:{PR_B}")
        review = {"author": {"login": "danielraffel"}, "authorAssociation": "OWNER",
                  "state": "APPROVED", "submittedAt": "2026-10-12T11:00:00Z",
                  "url": "r", "commit": {"oid": PR_B}}
        for held in (pr([hold_b, comment]), pr([hold_b], [review])):
            code, out = run(Fake({12: held, 11: pr([])}))
            self.assertEqual(code, 1, out)
            self.assertIn("#12 is held", out)
            self.assertIn("https://x/hold", out)
        for association in ("OWNER", "MEMBER", "COLLABORATOR"):
            approve = node("someone", association, "2026-10-12T11:00:00Z",
                           f"verdict: APPROVE head:{PR_B}")
            self.assertFalse(hold.latest_verdict(verdicts_of(pr([hold_b, approve])), PR_B)["approve"],
                             association)

    def test_only_a_trusted_verdict_on_that_head_counts(self) -> None:
        hold_b = f"verdict: HOLD head:{PR_B}"
        for comments, author in [
            ([node("drive-by", "NONE", "2026-10-12T10:00:00Z", hold_b)], "alice"),
            ([node("alice", "OWNER", "2026-10-12T10:00:00Z", f"verdict: APPROVE head:{PR_B}")],
             "alice"),
            ([node("daniel", "OWNER", "2026-10-12T10:00:00Z", f"verdict: HOLD head:{PR_A}")], "alice"),
            ([node("daniel", "OWNER", "2026-10-12T10:00:00Z", f"> verdict: HOLD head:{PR_B}")], "alice"),
        ]:
            self.assertIsNone(hold.latest_verdict(verdicts_of(pr(comments, author=author)), PR_B), comments)
        # The author's own HOLD counts, whatever their association; their
        # APPROVE never does, so it cannot clear a HOLD either.
        own_hold = pr([node("alice", "CONTRIBUTOR", "2026-10-12T10:00:00Z", hold_b)])
        self.assertFalse(hold.latest_verdict(verdicts_of(own_hold), PR_B)["approve"])
        self_cleared = pr([node("daniel", "OWNER", "2026-10-12T10:00:00Z", hold_b),
                           node("alice", "OWNER", "2026-10-12T11:00:00Z",
                                f"verdict: APPROVE head:{PR_B}")])
        self.assertFalse(hold.latest_verdict(verdicts_of(self_cleared), PR_B)["approve"])
        # The fleet App counts on its own pull request; a review decides too.
        self.assertFalse(hold.latest_verdict(verdicts_of(
            pr([node("shipyard-local[bot]", "NONE", "2026-10-12T10:00:00Z", hold_b)],
               author="shipyard-local")), PR_B)["approve"])
        review = {"author": {"login": "daniel"}, "authorAssociation": "MEMBER",
                  "state": "CHANGES_REQUESTED", "submittedAt": "2026-10-12T10:00:00Z",
                  "url": "r", "commit": {"oid": PR_B}}
        self.assertFalse(hold.latest_verdict(verdicts_of(pr([], [review])), PR_B)["approve"])
        # A tie goes to the HOLD.
        tie = pr([node("daniel", "OWNER", "2026-10-12T10:00:00Z", f"verdict: APPROVE head:{PR_B}"),
                  node("shipyard-local", "NONE", "2026-10-12T10:00:00Z", hold_b)])
        self.assertFalse(hold.latest_verdict(verdicts_of(tie), PR_B)["approve"])

    def test_shipyards_captured_10114_reads_hold(self) -> None:
        # Spelled as one repository path so test/ctest_script_inputs.json records it.
        answer = json.loads((ROOT / "tools/ci/fixtures/merge_group_hold/pr-10114-verdicts.json")
                            .read_text(encoding="utf-8"))
        pull = answer["data"]["repository"]["pullRequest"]
        verdict = hold.latest_verdict(pull, pull["headRefOid"])
        self.assertIsNotNone(verdict)
        self.assertFalse(verdict["approve"])

    def test_anything_unreadable_fails_closed_and_says_what_to_do(self) -> None:
        unreadable = "::error title=merge-group HOLD check (unreadable)::"
        cases = [
            ("a failed commit read", Fake({}, commits=OSError("HTTP 502")),
             [f"merge group {MERGE_B[:12]}", "HTTP 502"]),
            ("a chain that does not reach the base",
             Fake({}, commits={MERGE_B: commit(MERGE_B, [BASE, PR_B], "Squashed: something")}),
             [f"merge group {MERGE_B[:12]}", "not a queue merge commit"]),
            ("a failed GraphQL read", Fake({12: OSError("HTTP 403"), 11: pr([])}),
             ["#12: its verdicts could not be read", "HTTP 403",
              "https://github.com/o/r/pull/12"]),
            ("a GraphQL errors answer",
             Fake({12: {"errors": [{"message": "Resource not accessible by integration"}],
                        "data": None}, 11: pr([])}),
             ["#12: its verdicts could not be read", "Resource not accessible by integration"]),
            ("a list that stops short of its totalCount",
             Fake({12: pr([node("bot", "NONE", "2026-10-12T09:00:00Z", "noise")] * 100),
                   11: pr([])}, lie_total=150),
             ["#12: its verdicts could not be read", "read 100 of 150 comments"]),
        ]
        for name, fake, expected in cases:
            code, out = run(fake)
            self.assertEqual(code, 1, (name, out))
            self.assertIn(unreadable, out, name)
            self.assertIn("Fix: re-run the merge group's classify job", out, name)
            for text in expected:
                self.assertIn(text, out, name)
            self.assertNotIn("no queued pull request is held", out, name)

    def test_a_missing_token_fails_closed(self) -> None:
        saved = {key: os.environ.pop(key) for key in ("GH_TOKEN", "GITHUB_TOKEN")
                 if key in os.environ}
        try:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = hold.main(["--repo", "o/r", "--base", BASE, "--head", MERGE_B])
        finally:
            os.environ.update(saved)
        self.assertEqual(code, 1, out.getvalue())
        self.assertIn("no GH_TOKEN or GITHUB_TOKEN", out.getvalue())
        self.assertIn("Fix: hand the step the workflow token", out.getvalue())

    def test_a_hold_past_the_first_page_of_comments_is_read(self) -> None:
        noise = [node("bot", "NONE", "2026-10-12T09:00:00Z", "coverage: ok")] * 150
        held = pr(noise + [node("shipyard-local", "NONE", "2026-10-12T10:00:00Z",
                                f"verdict: HOLD head:{PR_B}", url="https://x/late-hold")])
        fake = Fake({12: held, 11: pr([])})
        code, out = run(fake)
        self.assertEqual(code, 1, out)
        self.assertIn("#12 is held", out)
        self.assertIn("https://x/late-hold", out)
        pages = [call for call in fake.calls if call[1] == "graphql"]
        self.assertGreaterEqual(len(pages), 3, "two comment pages and a review page for #12")

    def test_a_group_larger_than_the_compare_cap_is_read_in_full(self) -> None:
        # 260 queue merges: the compare API's 250-commit cap would hide some.
        # The entry nearest the base is the held one.
        commits = {}
        parent = BASE
        lands = {}
        for number in range(1, 261):
            merge = f"{number:040x}"
            landed = f"{number + 1000:040x}"
            commits[merge] = commit(merge, [parent, landed], f"Merge pull request #{number} from o/x")
            lands[number] = landed
            parent = merge
        head = parent
        verdicts = {number: pr([]) for number in lands}
        verdicts[1] = pr([node("daniel", "OWNER", "2026-10-12T10:00:00Z",
                               f"verdict: HOLD head:{lands[1]}")])
        code, out = run(Fake(verdicts, commits=commits), head=head)
        self.assertEqual(code, 1, out[-2000:])
        self.assertIn("#1 is held", out)
        self.assertNotIn("(unreadable)", out)

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

    def run_step(self, git_fetch: int, has_check: bool) -> subprocess.CompletedProcess:
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        step = workflow[workflow.index("- name: Refuse a merge group whose pull request is held"):]
        script = step[step.index("run: |\n") + len("run: |\n"):step.index("- id: classify")]
        script = "\n".join(line[10:] for line in script.splitlines())
        script = script.replace("${{ github.token }}", "token")
        with tempfile.TemporaryDirectory() as tmp:
            fake_git = pathlib.Path(tmp, "git")
            fake_git.write_text(
                "#!/bin/bash\n"
                "case \"$1\" in\n"
                f"  fetch) exit {git_fetch} ;;\n"
                f"  cat-file) exit {0 if has_check else 128} ;;\n"
                "  show) echo 'import sys; print(\"check ran\"); sys.exit(1)' ;;\n"
                "esac\n", encoding="utf-8")
            fake_git.chmod(0o755)
            env = dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}",
                       MERGE_GROUP_BASE_REF="refs/heads/main", MERGE_GROUP_BASE_SHA=BASE,
                       MERGE_GROUP_HEAD_SHA=MERGE_B, GITHUB_REPOSITORY="o/r")
            return subprocess.run(["bash", "-c", script], env=env, capture_output=True,
                                  text=True, encoding="utf-8", timeout=60)

    def test_the_step_fails_closed_unless_the_base_has_no_check(self) -> None:
        fetch_failed = self.run_step(git_fetch=1, has_check=True)
        self.assertEqual(fetch_failed.returncode, 1, fetch_failed.stdout)
        self.assertIn("::error title=merge-group HOLD check::could not fetch refs/heads/main",
                      fetch_failed.stdout)
        self.assertNotIn("check ran", fetch_failed.stdout)
        absent = self.run_step(git_fetch=0, has_check=False)
        self.assertEqual(absent.returncode, 0, absent.stdout)
        self.assertIn("::notice title=merge-group HOLD check::refs/heads/main has no HOLD check "
                      "yet", absent.stdout)
        present = self.run_step(git_fetch=0, has_check=True)
        self.assertEqual(present.returncode, 1, present.stdout + present.stderr)
        self.assertIn("check ran", present.stdout)


if __name__ == "__main__":
    unittest.main()
