#!/usr/bin/env python3
"""Tests for review_latency_watchdog.py.

The load-bearing behavior: a ready pull request with no verdict on its CURRENT
head alarms once it has waited past the threshold, measured from the latest of
open, ready-for-review and head push. A verdict on an older head does not
count, a draft never alarms, and a failed read is an alarm, never a clean bill.
"""
from __future__ import annotations

import datetime as dt
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import review_latency_watchdog as rlw  # noqa: E402

NOW = dt.datetime(2026, 10, 26, 12, 0, 0, tzinfo=dt.timezone.utc)
HEAD = "31a51ba5f6e061a0af9628c0137e8f332b2aaa17"
OLD = "4dab5df5aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"


def ago(minutes: float) -> str:
    return (NOW - dt.timedelta(minutes=minutes)).isoformat().replace("+00:00", "Z")


def pr(number=1, *, created=180, ready=None, head_at=90, draft=False, verdicts=()):
    return {
        "number": number,
        "title": f"PR {number}",
        "url": f"https://github.com/o/r/pull/{number}",
        "author": "agent",
        "is_draft": draft,
        "created_at": ago(created),
        "ready_at": ago(ready) if ready is not None else None,
        "head_oid": HEAD,
        "head_at": ago(head_at),
        "verdict_heads": list(verdicts),
    }


def alarms(prs, errors=(), threshold=60):
    findings = rlw.analyze({"prs": prs, "errors": list(errors)}, NOW, threshold)
    return findings


class PredicateTests(unittest.TestCase):
    def test_no_verdict_past_threshold_alarms(self):
        findings = alarms([pr(head_at=90)])
        self.assertEqual([f["number"] for f in findings], [1])
        self.assertEqual(findings[0]["level"], "review_latency_alarm")
        self.assertEqual(findings[0]["waited_minutes"], 90)

    def test_under_threshold_is_quiet(self):
        self.assertEqual(alarms([pr(head_at=59)]), [])

    def test_verdict_on_current_head_is_quiet(self):
        self.assertEqual(alarms([pr(verdicts=[HEAD])]), [])

    def test_short_sha_prefix_counts(self):
        self.assertEqual(alarms([pr(verdicts=[HEAD[:7]])]), [])

    def test_prefix_shorter_than_seven_does_not_count(self):
        self.assertEqual(len(alarms([pr(verdicts=[HEAD[:6]])])), 1)

    def test_verdict_on_older_head_still_alarms(self):
        findings = alarms([pr(verdicts=[OLD])])
        self.assertEqual(len(findings), 1)
        self.assertTrue(findings[0]["earlier_verdict"])

    def test_draft_never_alarms(self):
        self.assertEqual(alarms([pr(draft=True, head_at=600)]), [])

    def test_wait_starts_at_ready_for_review(self):
        # Opened as a draft 3 h ago, head pushed 2 h ago, marked ready 30 min ago.
        self.assertEqual(alarms([pr(created=180, head_at=120, ready=30)]), [])

    def test_wait_starts_at_latest_push(self):
        self.assertEqual(alarms([pr(created=600, head_at=10)]), [])

    def test_longest_wait_first(self):
        findings = alarms([pr(1, head_at=70), pr(2, head_at=300)])
        self.assertEqual([f["number"] for f in findings], [2, 1])

    def test_failed_read_is_an_alarm(self):
        findings = alarms([], errors=["graphql: HTTP 502"])
        self.assertEqual([f["level"] for f in findings], ["blind_alarm"])


class ParsingTests(unittest.TestCase):
    def test_verdict_lines(self):
        body = (
            "reviewer: pulp-review\n"
            "Some prose mentioning verdict: APPROVE head:deadbeef in passing.\n"
            f"verdict: APPROVE head:{HEAD}\n"
            f"  Verdict: hold head: {OLD[:12]}\n"
        )
        self.assertEqual(rlw.verdict_heads_in(body), [HEAD, OLD[:12]])

    def test_prose_without_head_is_not_a_verdict(self):
        self.assertEqual(rlw.verdict_heads_in("verdict: APPROVE\n"), [])

    def test_normalize_reads_comments_reviews_and_push_time(self):
        node = {
            "number": 7,
            "title": "t",
            "url": "u",
            "isDraft": False,
            "createdAt": ago(300),
            "headRefOid": HEAD,
            "author": {"login": "agent"},
            "timelineItems": {"nodes": [{"createdAt": ago(200)}]},
            "commits": {"nodes": [{"commit": {
                "oid": HEAD,
                "committedDate": ago(500),
                "checkSuites": {"nodes": [{"createdAt": ago(100)}]},
            }}]},
            "comments": {"nodes": [{"body": f"verdict: HOLD head:{OLD}"}]},
            "reviews": {"nodes": [
                {"state": "COMMENTED", "commit": {"oid": HEAD}},
                {"state": "APPROVED", "commit": {"oid": OLD}},
            ]},
        }
        got = rlw.normalize_pr(node)
        self.assertEqual(got["head_at"], ago(100))
        self.assertEqual(got["ready_at"], ago(200))
        self.assertEqual(got["verdict_heads"], [OLD, OLD])
        self.assertEqual(len(rlw.analyze({"prs": [got]}, NOW, 60)), 1)

    def test_normalize_falls_back_to_committed_date(self):
        node = {
            "number": 8,
            "headRefOid": HEAD,
            "createdAt": ago(30),
            "commits": {"nodes": [{"commit": {"committedDate": ago(10), "checkSuites": {"nodes": []}}}]},
        }
        self.assertEqual(rlw.normalize_pr(node)["head_at"], ago(10))

    def test_review_on_current_head_counts(self):
        node = {
            "number": 9,
            "headRefOid": HEAD,
            "createdAt": ago(300),
            "commits": {"nodes": [{"commit": {"committedDate": ago(300)}}]},
            "reviews": {"nodes": [{"state": "CHANGES_REQUESTED", "commit": {"oid": HEAD}}]},
        }
        self.assertEqual(rlw.analyze({"prs": [rlw.normalize_pr(node)]}, NOW, 60), [])


class CollectionTests(unittest.TestCase):
    def test_collection_failure_is_recorded(self):
        err = subprocess.CalledProcessError(1, ["gh"], "", "HTTP 401: Bad credentials")
        with mock.patch.object(rlw, "_gh", side_effect=err):
            snapshot = rlw.collect_snapshot("o/r", "main", NOW)
        self.assertEqual(snapshot["prs"], [])
        self.assertEqual(len(snapshot["errors"]), 1)
        self.assertIn("Bad credentials", snapshot["errors"][0])

    def test_pagination(self):
        def page(nodes, more, cursor=None):
            return json.dumps({"data": {"repository": {"pullRequests": {
                "pageInfo": {"hasNextPage": more, "endCursor": cursor},
                "nodes": nodes,
            }}}})
        node = {"number": 1, "headRefOid": HEAD, "createdAt": ago(10)}
        calls = []

        def fake(args):
            calls.append(args)
            return page([node], True, "c1") if len(calls) == 1 else page([dict(node, number=2)], False)

        with mock.patch.object(rlw, "_gh", side_effect=fake):
            snapshot = rlw.collect_snapshot("o/r", "main", NOW)
        self.assertEqual([p["number"] for p in snapshot["prs"]], [1, 2])
        self.assertIn("cursor=c1", calls[1])


class CliTests(unittest.TestCase):
    def test_replay_writes_body_only_when_findings(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            snap = tmp_path / "snap.json"
            snap.write_text(json.dumps({"generated_at": NOW.isoformat(), "prs": [pr(head_at=90)], "errors": []}))
            body = tmp_path / "body.md"
            findings = tmp_path / "findings.json"
            self.assertEqual(rlw.main([
                "--snapshot", str(snap), "--findings-out", str(findings), "--body-out", str(body),
            ]), 0)
            self.assertIn("#1", body.read_text())
            self.assertEqual(len(json.loads(findings.read_text())), 1)

            snap.write_text(json.dumps({"generated_at": NOW.isoformat(), "prs": [pr(verdicts=[HEAD])], "errors": []}))
            body.unlink()
            rlw.main(["--snapshot", str(snap), "--findings-out", str(findings), "--body-out", str(body)])
            self.assertFalse(body.exists())


class WiringTests(unittest.TestCase):
    ROOT = Path(__file__).resolve().parents[2]

    def test_workflow_lint_runs_these_tests(self):
        lint = (self.ROOT / ".github" / "workflows" / "workflow-lint.yml").read_text(encoding="utf-8")
        self.assertIn("python3 tools/scripts/test_review_latency_watchdog.py", lint)
        self.assertIn("'tools/scripts/review_latency_watchdog.py'", lint)

    def test_merge_stall_workflow_runs_the_scan_and_maintains_its_tracker(self):
        wf = (self.ROOT / ".github" / "workflows" / "merge-stall-check.yml").read_text(encoding="utf-8")
        self.assertIn("python3 tools/scripts/review_latency_watchdog.py", wf)
        self.assertIn('maintain_tracker "${REVIEW_TRACKING_TITLE}"', wf)


if __name__ == "__main__":
    unittest.main()
