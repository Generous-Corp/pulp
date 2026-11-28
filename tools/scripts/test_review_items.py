#!/usr/bin/env python3
"""Tests for review_items.py against a real git repository and recorded comments."""
from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import review_items as ri  # noqa: E402

GIT_ENV = {"PATH": "/usr/bin:/bin:/opt/homebrew/bin:/usr/local/bin", "HOME": "/nonexistent",
           "GIT_CONFIG_NOSYSTEM": "1", "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}
HEAVY = "core/a/include/pulp/a/heavy.hpp"
UMBRELLA = "core/a/include/pulp/a/umbrella.hpp"
CENSUS = "docs/status/consumption-profiles.json"
AGGREGATE = "test/agg.json"
FANOUT_LEDGER = "tools/scripts/header_fanout_guard.json"
HOTSPOT_LEDGER = "tools/scripts/hotspot_edit_guard.json"
HOTSPOT_ID = f"hotspot:{CENSUS}"
FANOUT_ID = f"fanout:{HEAVY}:+30"
CONFIG = """[auto_merge]
reviewer_signers = ["pulp-review", "infra-review2"]
reviewer_stamp_sessions = ["fleet-pass-reviewer-*"]
"""
REVIEWER_STAMP = ("<!-- session: Daniels-Mac-Studio-m3/fleet-pass-reviewer-m3-20261126T045514Z-1 "
                  "agent: pulp-review -->")


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], env=GIT_ENV, capture_output=True,
                          text=True, encoding="utf-8", check=True).stdout


def write(root: Path, rel: str, text: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def fixture(root: Path) -> None:
    """main with both ledgers; the branch regenerates the census and adds an umbrella include."""
    git(root, "init", "-q", "-b", "main")
    for key, value in (("maintenance.auto", "false"), ("gc.auto", "0"), ("commit.gpgsign", "false")):
        git(root, "config", key, value)
    write(root, "tools/scripts/hotspot_edit_guard.json", json.dumps({
        "schema": "pulp-hotspot-edit-guard/v1",
        "hotspots": [{"mode": "serialized", "path": CENSUS, "writer": "census --write"},
                     {"path": AGGREGATE, "fragments": "test/agg/", "writer": "agg --write"}]}))
    write(root, "tools/scripts/header_fanout_guard.json", json.dumps({
        "schema_version": 1, "headers": [{"path": HEAVY, "max_tus": 5}]}))
    write(root, ".shipyard/config.toml", CONFIG)
    write(root, CENSUS, "{}\n")
    write(root, HEAVY, "#pragma once\nstruct Heavy {};\n")
    write(root, UMBRELLA, "#pragma once\nstruct Umbrella {};\n")
    for i in range(30):
        write(root, f"test/test_u{i}.cpp", '#include <pulp/a/umbrella.hpp>\n')
    write(root, "core/a/src/heavy.cpp", '#include "pulp/a/heavy.hpp"\n')
    git(root, "add", ".")
    git(root, "commit", "-q", "-m", "base")
    git(root, "checkout", "-q", "-b", "topic")


def grow(root: Path, message: str = "regenerate the census and add an umbrella include") -> str:
    write(root, CENSUS, '{"changed": true}\n')
    write(root, UMBRELLA, "#pragma once\n#include <pulp/a/heavy.hpp>\n")
    git(root, "add", ".")
    git(root, "commit", "-q", "-m", message)
    return git(root, "rev-parse", "HEAD").strip()


def verdict(head: str, *accepts: str, login: str = "shipyard-local[bot]", kind: str = "Bot",
            stamp: str = REVIEWER_STAMP, signer: str = "pulp-review", comment_id: int = 1,
            created: str = "2026-11-27T10:00:00Z", updated: str | None = None,
            extra: str = "") -> dict:
    body = "Reviewed.\n" + extra + "".join(f"accept-review-item: {a}\n" for a in accepts)
    body += f"\nreviewer: {signer}\n{stamp}\nverdict: APPROVE head:{head}"
    return {"id": comment_id, "user": {"login": login, "type": kind}, "body": body,
            "created_at": created, "updated_at": updated or created}


class RepoCase(unittest.TestCase):
    def setUp(self) -> None:
        self._td = tempfile.TemporaryDirectory()
        self.addCleanup(self._td.cleanup)
        self.root = Path(self._td.name) / "repo"
        self.root.mkdir()
        fixture(self.root)

    def check(self, head: str, comments: list[dict], *extra: str) -> tuple[int, str]:
        path = Path(self._td.name) / "comments.json"
        path.write_text(json.dumps(comments), encoding="utf-8")
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            rc = ri.main(["review_items.py", "check", "--root", str(self.root), "--base", "main",
                          "--head", head, "--repo", "o/r", "--pr", "7",
                          "--comments-json", str(path), *extra])
        return rc, out.getvalue()


class CheckCli(RepoCase):
    def test_the_branch_has_both_review_items(self) -> None:
        head = grow(self.root)
        items, failures = ri.collect_items(self.root, "main", head)
        self.assertEqual(sorted(i["id"] for i in items), sorted([HOTSPOT_ID, FANOUT_ID]))
        self.assertEqual(failures, [])

    def test_an_unacknowledged_item_fails(self) -> None:
        head = grow(self.root)
        rc, out = self.check(head, [verdict(head)])
        self.assertEqual(rc, 1, out)
        self.assertIn(HOTSPOT_ID, out)
        self.assertIn(FANOUT_ID, out)

    def test_a_partial_acknowledgement_fails_on_the_rest(self) -> None:
        head = grow(self.root)
        rc, out = self.check(head, [verdict(head, HOTSPOT_ID)])
        self.assertEqual(rc, 1, out)
        self.assertIn(f"unaccepted: {FANOUT_ID}", out)

    def test_an_acknowledged_item_passes(self) -> None:
        head = grow(self.root)
        rc, out = self.check(head, [verdict(head, HOTSPOT_ID, FANOUT_ID)])
        self.assertEqual(rc, 0, out)

    def test_an_ack_from_a_non_reviewer_identity_is_ignored(self) -> None:
        head = grow(self.root)
        accepts = (HOTSPOT_ID, FANOUT_ID)
        cases = {
            "a human account": verdict(head, *accepts, login="danielraffel", kind="User"),
            "a user account named like the App": verdict(head, *accepts, kind="User"),
            "the Actions bot (pull request code)": verdict(head, *accepts, login="github-actions[bot]"),
            "an author session": verdict(head, *accepts, stamp=REVIEWER_STAMP.replace(
                "fleet-pass-reviewer-", "fleet-author-")),
            "no session stamp": verdict(head, *accepts, stamp=""),
            "a stamp-like variant": verdict(head, *accepts, stamp=REVIEWER_STAMP.replace(
                "<!-- session:", "<!--  session:")),
            "an unlisted signer": verdict(head, *accepts, signer="someone-else"),
            "an edited comment": verdict(head, *accepts, updated="2026-11-27T11:00:00Z"),
        }
        for name, comment in cases.items():
            with self.subTest(name):
                rc, out = self.check(head, [comment])
                self.assertEqual(rc, 1, out)
                self.assertIn("no qualifying reviewer verdict", out)

    def test_an_ack_for_a_different_head_is_ignored(self) -> None:
        head = grow(self.root)
        other = "0" * 40
        rc, out = self.check(head, [verdict(other, HOTSPOT_ID, FANOUT_ID)])
        self.assertEqual(rc, 1, out)
        # The same acknowledgement on an earlier head of the same branch does
        # not carry to the next one either.
        write(self.root, "README.md", "x\n")
        git(self.root, "add", "README.md")
        git(self.root, "commit", "-q", "-m", "another commit")
        later = git(self.root, "rev-parse", "HEAD").strip()
        rc, out = self.check(later, [verdict(head, HOTSPOT_ID, FANOUT_ID)])
        self.assertEqual(rc, 1, out)

    def test_the_trailers_still_accept_without_any_verdict(self) -> None:
        head = grow(self.root, "regenerate\n\nSerialized-Hotspot: " + CENSUS + " reason=single writer\n"
                    f"Fanout-Grow: {HEAVY} reason=\"intended\"")
        rc, out = self.check(head, [])
        self.assertEqual(rc, 0, out)
        self.assertIn("accepted by a commit trailer", out)

    def test_quoted_or_fenced_acks_do_not_count(self) -> None:
        head = grow(self.root)
        lines = f"accept-review-item: {HOTSPOT_ID}\naccept-review-item: {FANOUT_ID}\n"
        quoted = "".join("> " + line + "\n" for line in lines.splitlines())
        for name, pasted in (("fenced", "```\n" + lines + "```\n"), ("quoted", quoted)):
            with self.subTest(name):
                rc, out = self.check(head, [verdict(head, extra=pasted)])
                self.assertEqual(rc, 1, out)
        # The same lines outside the fence do count.
        rc, out = self.check(head, [verdict(head, extra="```\nlog\n```\n" + lines)])
        self.assertEqual(rc, 0, out)

    def test_the_newest_qualifying_verdict_decides(self) -> None:
        head = grow(self.root)
        older = verdict(head, HOTSPOT_ID, FANOUT_ID, comment_id=1, created="2026-11-27T10:00:00Z")
        newer = verdict(head, comment_id=2, created="2026-11-27T12:00:00Z")
        rc, out = self.check(head, [older, newer])
        self.assertEqual(rc, 1, out)
        rc, out = self.check(head, [newer | {"id": 1, "created_at": "2026-11-27T09:00:00Z",
                                             "updated_at": "2026-11-27T09:00:00Z"},
                                    older | {"id": 2}])
        self.assertEqual(rc, 0, out)

    def test_a_base_config_without_reviewer_keys_fails_closed(self) -> None:
        head = grow(self.root)
        write(self.root, ".shipyard/config.toml", "[auto_merge]\nreviewer_signers = [\"pulp-review\"]\n")
        rc, out = self.check(head, [verdict(head, HOTSPOT_ID, FANOUT_ID)])
        self.assertEqual(rc, 2, out)
        self.assertIn("reviewer_stamp_sessions", out)

    def test_a_short_head_is_refused(self) -> None:
        head = grow(self.root)
        rc, out = self.check(head[:12], [])
        self.assertEqual(rc, 2, out)

    def test_post_status_posts_failure_and_writes_the_marker(self) -> None:
        head = grow(self.root)
        marker = Path(self._td.name) / "posted"
        posted: list[list[str]] = []
        original = ri.gh_json
        ri.gh_json = lambda args: posted.append(args) or {}
        try:
            rc, out = self.check(head, [verdict(head, HOTSPOT_ID, FANOUT_ID)], "--post-status",
                                 "--posted-marker", str(marker))
            self.assertEqual(rc, 0, out)
            self.assertFalse(marker.exists(), "an accepted head posts nothing")
            rc, out = self.check(head, [verdict(head)], "--post-status", "--posted-marker", str(marker))
            self.assertEqual(rc, 1, out)
        finally:
            ri.gh_json = original
        self.assertTrue(marker.exists(), out)
        self.assertEqual(len(posted), 1)
        args = posted[0]
        self.assertIn(f"repos/o/r/statuses/{head}", args)
        self.assertIn("state=failure", args)
        self.assertIn("context=review-verdict", args)
        description = next(a for a in args if a.startswith("description="))
        self.assertLessEqual(len(description) - len("description="), ri.DESCRIPTION_LIMIT)

    def test_a_failed_post_writes_no_marker(self) -> None:
        head = grow(self.root)
        marker = Path(self._td.name) / "posted"

        def refuse(args: list[str]) -> None:
            raise ri.ReviewItemError("HTTP 500")

        original = ri.gh_json
        ri.gh_json = refuse
        try:
            rc, out = self.check(head, [verdict(head)], "--post-status", "--posted-marker", str(marker))
        finally:
            ri.gh_json = original
        self.assertEqual(rc, 2, out)
        self.assertFalse(marker.exists(), out)

class HardFailures(RepoCase):
    """A guard's outright refusal fails the check whatever the verdict says."""

    def commit(self, message: str = "change") -> str:
        git(self.root, "add", "--all")
        git(self.root, "commit", "-q", "-m", message)
        return git(self.root, "rev-parse", "HEAD").strip()

    def accept_all(self, head: str) -> list[dict]:
        items, _ = ri.collect_items(self.root, "main", head)
        return [verdict(head, *[i["id"] for i in items])]

    def test_a_missing_tracked_header_fails_even_when_every_item_is_accepted(self) -> None:
        git(self.root, "rm", "-q", HEAVY)
        head = self.commit("remove the heavy header but keep its ledger entry")
        rc, out = self.check(head, self.accept_all(head))
        self.assertEqual(rc, 1, out)
        self.assertIn("no longer exists", out)

    def test_a_fragment_hotspot_edit_fails_even_when_every_item_is_accepted(self) -> None:
        write(self.root, AGGREGATE, "{}\n")
        head = self.commit("re-add the split aggregate")
        rc, out = self.check(head, self.accept_all(head))
        self.assertEqual(rc, 1, out)
        self.assertIn(f"hotspot edit refused: {AGGREGATE}", out)

    def test_a_ledger_edit_cannot_hide_a_fragment_hotspot_edit(self) -> None:
        # The head drops the aggregate from its own registry, so the head's
        # guard passes; the default branch's registry still refuses it.
        write(self.root, HOTSPOT_LEDGER, json.dumps({
            "schema": "pulp-hotspot-edit-guard/v1",
            "hotspots": [{"mode": "serialized", "path": CENSUS, "writer": "census --write"}]}))
        write(self.root, AGGREGATE, "{}\n")
        head = self.commit("unregister the aggregate and edit it")
        rc, out = self.check(head, self.accept_all(head))
        self.assertEqual(rc, 1, out)
        self.assertIn(f"hotspot edit refused: {AGGREGATE}", out)

    def test_a_raised_ceiling_in_the_head_ledger_is_still_measured_against_main(self) -> None:
        write(self.root, FANOUT_LEDGER, json.dumps({
            "schema_version": 1, "headers": [{"path": HEAVY, "max_tus": 500}]}))
        head = grow(self.root, "raise the ceiling and add an umbrella include")
        rc, out = self.check(head, [verdict(head, HOTSPOT_ID, f"ledger:{FANOUT_LEDGER}")])
        self.assertEqual(rc, 1, out)
        self.assertIn(f"unaccepted: {FANOUT_ID}", out)

    def test_removing_a_header_with_its_ledger_entry_is_a_ledger_review_item(self) -> None:
        git(self.root, "rm", "-q", HEAVY, "core/a/src/heavy.cpp")
        write(self.root, FANOUT_LEDGER, json.dumps({
            "schema_version": 1, "headers": [{"path": UMBRELLA, "max_tus": 100}]}))
        head = self.commit("remove the heavy header and its ledger entry")
        rc, out = self.check(head, [verdict(head)])
        self.assertEqual(rc, 1, out)
        self.assertIn(f"unaccepted: ledger:{FANOUT_LEDGER}", out)
        rc, out = self.check(head, [verdict(head, f"ledger:{FANOUT_LEDGER}")])
        self.assertEqual(rc, 0, out)

    def test_the_ledgers_come_from_the_base_ref_not_the_working_tree(self) -> None:
        head = grow(self.root)
        # An uncommitted, looser ledger in the checkout changes nothing.
        write(self.root, FANOUT_LEDGER, json.dumps({
            "schema_version": 1, "headers": [{"path": HEAVY, "max_tus": 500}]}))
        items, _ = ri.collect_items(self.root, "main", head)
        self.assertIn(FANOUT_ID, [i["id"] for i in items])

    def test_a_hard_failure_posts_a_failure_status(self) -> None:
        write(self.root, AGGREGATE, "{}\n")
        head = self.commit("re-add the split aggregate")
        posted: list[list[str]] = []
        original = ri.gh_json
        ri.gh_json = lambda args: posted.append(args) or {}
        try:
            rc, out = self.check(head, [], "--post-status")
        finally:
            ri.gh_json = original
        self.assertEqual(rc, 1, out)
        self.assertEqual(len(posted), 1, out)
        self.assertIn("state=failure", posted[0])


class ItemsRoundTrip(RepoCase):
    def test_the_printed_lines_pasted_into_a_verdict_satisfy_check(self) -> None:
        head = grow(self.root)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = ri.main(["review_items.py", "items", "--root", str(self.root), "--base", "main",
                          "--head", head])
        self.assertEqual(rc, 0, out.getvalue())
        pasted = "".join(line + "\n" for line in out.getvalue().splitlines()
                         if line.startswith("accept-review-item:"))
        self.assertEqual(pasted.count("accept-review-item:"), 2, out.getvalue())
        rc, check_out = self.check(head, [verdict(head, extra=pasted)])
        self.assertEqual(rc, 0, out.getvalue() + check_out)


class Parsing(unittest.TestCase):
    def test_gh_json_flattens_paginated_arrays(self) -> None:
        original = subprocess.run
        subprocess.run = lambda *a, **k: subprocess.CompletedProcess(a, 0, '[{"id":1}]\n[{"id":2}]', "")
        try:
            self.assertEqual(ri.gh_json(["x"]), [{"id": 1}, {"id": 2}])
        finally:
            subprocess.run = original

    def test_glob_match_stays_within_one_segment(self) -> None:
        self.assertTrue(ri.glob_match("fleet-pass-reviewer-*", "fleet-pass-reviewer-m3-1"))
        self.assertFalse(ri.glob_match("fleet-pass-reviewer-*", "fleet-pass-reviewer-x/evil"))
        self.assertFalse(ri.glob_match("fleet-pass-reviewer-*", "author-fleet-pass-reviewer-1"))


if __name__ == "__main__":
    unittest.main()
