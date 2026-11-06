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
  and only a base without the check passes, with a notice;
- the step scans from main's tip (the "Resolve the merge group's main base"
  step), not merge_group.base_sha: on a real queue chain a HOLD on an entry
  ahead in the batch fails the group that lands it, a clean batch passes, and
  a base that did not resolve fails the group before the check runs.

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
from merge_queue_fixture import MergeQueueRepo  # noqa: E402

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

    def test_a_hold_from_a_user_account_still_counts(self) -> None:
        # Only an APPROVE needs the Bot: a HOLD from a User-type account (the
        # owner, a member, the PR's author, or a user named like the App) can
        # only stop a landing.
        approve = node("shipyard-local", "CONTRIBUTOR", "2026-10-12T10:00:00Z",
                       f"verdict: APPROVE head:{PR_B}")
        for login, association, author in (("danielraffel", "OWNER", "alice"),
                                           ("someone", "MEMBER", "alice"),
                                           ("alice", "CONTRIBUTOR", "alice"),
                                           # A user registered as "shipyard-local", with no
                                           # association, on someone else's pull request.
                                           ("shipyard-local", "NONE", "alice")):
            held = pr([approve, node(login, association, "2026-10-12T11:00:00Z",
                                     f"verdict: HOLD head:{PR_B}", url="https://x/hold",
                                     kind="User")], author=author)
            code, out = run(Fake({12: held, 11: pr([])}))
            self.assertEqual(code, 1, (login, out))
            self.assertIn("#12 is held", out)
        review = {"author": {"__typename": "User", "login": "danielraffel"},
                  "authorAssociation": "OWNER", "state": "CHANGES_REQUESTED",
                  "submittedAt": "2026-10-12T11:00:00Z", "url": "r", "commit": {"oid": PR_B}}
        self.assertFalse(hold.latest_verdict(verdicts_of(pr([approve], [review])), PR_B)["approve"])

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
        # main's tip, never base_sha: base_sha is the previous speculative
        # group when entries are queued ahead.
        self.assertIn("MERGE_GROUP_MAIN_BASE: ${{ steps.merge_group_base.outputs.base }}", step)
        self.assertIn('--base "$MERGE_GROUP_MAIN_BASE"', step)
        self.assertNotIn("github.event.merge_group.base_sha", step)
        self.assertLess(classify.index("id: merge_group_base"),
                        classify.index("- name: Refuse a merge group whose pull request is held"))
        self.assertNotIn("continue-on-error", step)
        # The token reaches only the protected base branch's copy of the
        # check, never the queue entry's checkout.
        self.assertIn('git show "FETCH_HEAD:tools/ci/merge_group_hold_check.py"', step)
        self.assertIn('GH_TOKEN="${{ github.token }}" python3 "$hold_check"', step)
        self.assertNotIn("GH_TOKEN: ${{ github.token }}", step)
        self.assertNotIn("python3 tools/ci/merge_group_hold_check.py", step)

    def run_step(self, git_fetch: int, has_check: bool,
                 main_base: str = BASE) -> subprocess.CompletedProcess:
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
                "  show) echo 'import sys; print(\"check ran\", *sys.argv[1:]); sys.exit(1)' ;;\n"
                "esac\n", encoding="utf-8")
            fake_git.chmod(0o755)
            env = dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}",
                       MERGE_GROUP_BASE_REF="refs/heads/main", MERGE_GROUP_MAIN_BASE=main_base,
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
        self.assertIn(f"check ran --repo o/r --base {BASE} --head {MERGE_B}", present.stdout)

    def test_an_unresolved_main_base_fails_closed_before_the_check(self) -> None:
        # The resolve step prints origin/main when the chain does not reach
        # main; the step must not fall back to a narrower scan.
        for main_base in ("origin/main", ""):
            with self.subTest(main_base=main_base):
                unresolved = self.run_step(git_fetch=0, has_check=True, main_base=main_base)
                self.assertEqual(unresolved.returncode, 1, unresolved.stdout)
                self.assertIn("::error title=merge-group HOLD check::main's tip could not be "
                              "resolved", unresolved.stdout)
                self.assertNotIn("check ran", unresolved.stdout)


class StackedQueueHoldCheck(unittest.TestCase):
    """A real batch of three entries, #11 ahead of #12 ahead of #13.

    #13's group has `merge_group.base_sha` = #12's speculative group, and it
    lands all three. The base the workflow hands the check comes from running
    build.yml's resolve step in a depth-1 checkout of that group.
    """

    def setUp(self) -> None:
        self.queue = MergeQueueRepo([11, 12, 13])
        self.addCleanup(self.queue.close)

    def check(self, verdicts: dict, base: str) -> tuple:
        fetch = self.queue.fetch("o/r", Fake(verdicts, commits={}))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = hold.main(["--repo", "o/r", "--base", base,
                              "--head", self.queue.groups[13]], fetch=fetch)
        return code, out.getvalue()

    def test_the_resolve_step_hands_the_last_group_mains_tip(self) -> None:
        self.assertNotEqual(self.queue.base_sha(13), self.queue.main)
        self.assertEqual(self.queue.resolve_main_base(13), self.queue.main)

    def test_a_hold_on_an_earlier_entry_fails_the_group_that_lands_it(self) -> None:
        held = pr([node("shipyard-local", "CONTRIBUTOR", "2026-11-05T10:00:00Z",
                        f"verdict: HOLD head:{self.queue.heads[11]}", url="https://x/hold")])
        verdicts = {11: held, 12: pr([]), 13: pr([])}
        code, out = self.check(verdicts, self.queue.resolve_main_base(13))
        self.assertEqual(code, 1, out)
        self.assertIn("::error title=merge-group HOLD check::#11 is held", out)
        self.assertIn("https://x/hold", out)
        # The negative control: from base_sha the same group only scans #13,
        # so the HOLD two entries ahead goes unchecked. That is the bypass.
        code, out = self.check(verdicts, self.queue.base_sha(13))
        self.assertEqual(code, 0, out)
        self.assertNotIn("#11", out)

    def test_a_clean_batch_passes_and_every_entry_was_read(self) -> None:
        verdicts = {n: pr([node("shipyard-local[bot]", "CONTRIBUTOR", "2026-11-05T10:00:00Z",
                                f"verdict: APPROVE head:{self.queue.heads[n]}")])
                    for n in (11, 12, 13)}
        fake = Fake(verdicts, commits={})
        fetch = self.queue.fetch("o/r", fake)
        base = self.queue.resolve_main_base(13)
        self.assertEqual(hold.group_entries(fetch, "o/r", base, self.queue.groups[13]),
                         [(13, self.queue.heads[13]), (12, self.queue.heads[12]),
                          (11, self.queue.heads[11])])
        code, out = self.check(verdicts, base)
        self.assertEqual(code, 0, out)
        self.assertIn("no queued pull request is held", out)

    def test_a_main_off_the_chain_resolves_to_the_fail_closed_fallback(self) -> None:
        # A base ref that carries the resolver but whose tip the group's
        # chain never reaches (main moved off the chain).
        q = self.queue
        q.git("checkout", "-q", "--detach", q.main)
        q.git("commit", "-q", "--allow-empty", "-m", "main moved")
        q.git("push", "-q", "origin", "HEAD:refs/heads/other")
        self.assertEqual(q.resolve_main_base(13, "refs/heads/other"), "origin/main")
        # The positive control on the same fixture: main itself resolves.
        self.assertEqual(q.resolve_main_base(12), q.main)


if __name__ == "__main__":
    unittest.main()
