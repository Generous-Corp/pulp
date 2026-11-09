#!/usr/bin/env python3
"""Tests for tools/ci/flake_quarantine.py (auto-quarantine of repeatedly excused flakes).

All GitHub reads and writes are stubbed. What must hold:
- 3 excusals of a test in 7 days quarantine it; 2 do not, and an excusal older
  than 7 days does not count; one run excusing a test twice counts once;
- a quarantine is active only while its issue is open, labelled, opened by the
  bot, and inside [start, start + 14 days); a marker edited to a longer span
  is not a quarantine: it expires after 14 days, a closed
  issue or a removed label ends it, and a non-bot issue never activates one;
- an expired, still-labelled issue is swept: comment and label removal;
- excusals before a test's latest quarantine began never count again, so a
  test closed as fixed does not re-enter on stale evidence;
- `active` writes the quarantined names and an anchored, escaped exclude
  fragment; a test the change touches stays in the required lane; an
  unreadable issue list and the shadow kill switch quarantine nothing;
- `sweep` never writes in shadow mode.

Run:
    python3 tools/ci/test_flake_quarantine.py
"""
from __future__ import annotations

import argparse
import datetime as dt
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import flake_exoneration as fx  # noqa: E402
import flake_quarantine as fq  # noqa: E402

NOW = dt.datetime(2026, 11, 9, 12, 0, tzinfo=dt.timezone.utc)
FLAKY = "Mixer meter settles under load (x|y)"


def rec(run_id: str, days_ago: float, *tests: str) -> dict:
    at = NOW - dt.timedelta(days=days_ago)
    return {"schema": fx.SCHEMA, "run_id": run_id, "excused_at": fq._iso(at),
            "tests": [{"name": t, "other_prs": [1, 2]} for t in tests]}


def issue(number: int, test: str, started_days_ago: float, *, state: str = "open",
          labels=(fq.FLAKE_LABEL, fq.QUARANTINE_LABEL), author: str = fq.BOT_LOGIN) -> dict:
    start = NOW - dt.timedelta(days=started_days_ago)
    return {"number": number, "state": state, "user": {"login": author},
            "labels": [{"name": l} for l in labels],
            "body": "Quarantined.\n\n" + fq.marker(test, start)}


class CountAndPlanTests(unittest.TestCase):
    def test_three_excusals_in_seven_days_quarantine(self) -> None:
        records = [rec("1", 0.5, FLAKY), rec("2", 3, FLAKY), rec("3", 6.5, FLAKY)]
        acts = fq.plan_sweep(records, [], NOW)
        self.assertEqual([(a.kind, a.test, a.issue, a.runs) for a in acts],
                         [("quarantine", FLAKY, None, ("1", "2", "3"))])

    def test_two_excusals_do_not(self) -> None:
        self.assertEqual(fq.plan_sweep([rec("1", 1, FLAKY), rec("2", 2, FLAKY)], [], NOW), [])

    def test_an_excusal_older_than_seven_days_does_not_count(self) -> None:
        records = [rec("1", 1, FLAKY), rec("2", 2, FLAKY), rec("3", 7.5, FLAKY)]
        self.assertEqual(fq.plan_sweep(records, [], NOW), [])

    def test_one_run_counts_once(self) -> None:
        records = [rec("1", 1, FLAKY), rec("1", 1, FLAKY), rec("2", 2, FLAKY)]
        self.assertEqual(fq.plan_sweep(records, [], NOW), [])

    def test_already_quarantined_is_left_alone(self) -> None:
        records = [rec(str(i), i, FLAKY) for i in range(1, 4)]
        self.assertEqual(fq.plan_sweep(records, [issue(7, FLAKY, 5)], NOW), [])

    def test_excusals_before_the_latest_quarantine_never_count_again(self) -> None:
        records = [rec("1", 3, FLAKY), rec("2", 2.5, FLAKY), rec("3", 2.2, FLAKY)]
        closed_fixed = issue(7, FLAKY, 2, state="closed")
        self.assertEqual(fq.plan_sweep(records, [closed_fixed], NOW), [])
        records += [rec("4", 1, FLAKY), rec("5", 0.5, FLAKY), rec("6", 0.1, FLAKY)]
        acts = fq.plan_sweep(records, [closed_fixed], NOW)
        self.assertEqual([(a.kind, a.issue, a.runs) for a in acts], [("quarantine", None, ("4", "5", "6"))])

    def test_requarantine_reuses_the_open_issue(self) -> None:
        expired = issue(7, FLAKY, 20, labels=(fq.FLAKE_LABEL,))
        records = [rec(str(i), i * 0.5, FLAKY) for i in range(1, 4)]
        acts = fq.plan_sweep(records, [expired], NOW)
        self.assertEqual([(a.kind, a.issue) for a in acts], [("quarantine", 7)])

    def test_foreign_schema_records_are_ignored(self) -> None:
        records = [dict(rec(str(i), 1, FLAKY), schema="other/v1") for i in range(3)]
        self.assertEqual(fq.plan_sweep(records, [], NOW), [])


class ExpiryTests(unittest.TestCase):
    def test_active_inside_fourteen_days_and_expired_after(self) -> None:
        self.assertEqual(fq.active_tests([issue(7, FLAKY, 13.9)], NOW), {FLAKY})
        self.assertEqual(fq.active_tests([issue(7, FLAKY, 14)], NOW), set())
        self.assertEqual(fq.active_tests([issue(7, FLAKY, 14.1)], NOW), set())

    def test_expired_labelled_issue_is_swept(self) -> None:
        acts = fq.plan_sweep([], [issue(7, FLAKY, 15)], NOW)
        self.assertEqual([(a.kind, a.issue) for a in acts], [("expire", 7)])
        self.assertEqual(fq.plan_sweep([], [issue(7, FLAKY, 10)], NOW), [])

    def test_closed_unlabelled_or_foreign_issue_is_not_active(self) -> None:
        self.assertEqual(fq.active_tests([issue(7, FLAKY, 1, state="closed")], NOW), set())
        self.assertEqual(fq.active_tests([issue(7, FLAKY, 1, labels=(fq.FLAKE_LABEL,))], NOW), set())
        self.assertEqual(fq.active_tests([issue(7, FLAKY, 1, author="someone")], NOW), set())

    def test_a_marker_edited_past_fourteen_days_is_not_a_quarantine(self) -> None:
        long = issue(7, FLAKY, 1)
        long["body"] = fq.marker(FLAKY, NOW - dt.timedelta(days=1), days=400)
        self.assertEqual(fq.active_tests([long], NOW), set())
        future = issue(7, FLAKY, -1)
        self.assertEqual(fq.active_tests([future], NOW), set())

    def test_a_body_without_a_valid_marker_is_ignored(self) -> None:
        bad = issue(7, FLAKY, 1)
        bad["body"] = "<!-- pulp-flake-quarantine/v1 {not json} -->"
        self.assertEqual(fq.active_tests([bad], NOW), set())
        self.assertEqual(fq.active_tests([dict(issue(7, FLAKY, 1), pull_request={})], NOW), set())

    def test_apply_writes_the_expected_calls(self) -> None:
        calls = []
        acts = [fq.Action("expire", FLAKY, 7), fq.Action("quarantine", FLAKY, None, ("1", "2", "3"))]
        self.assertEqual(fq.apply("o/r", acts, NOW, write=lambda m, p, b: calls.append((m, p, b)) or True), 0)
        self.assertEqual([(m, p) for m, p, _ in calls], [
            ("POST", "repos/o/r/issues/7/comments"),
            ("DELETE", f"repos/o/r/issues/7/labels/{fq.QUARANTINE_LABEL}"),
            ("POST", "repos/o/r/issues")])
        body = calls[2][2]
        self.assertEqual(sorted(body["labels"]), sorted([fq.FLAKE_LABEL, fq.QUARANTINE_LABEL]))
        q = fq.parse_issue({"number": 1, "state": "open", "user": {"login": fq.BOT_LOGIN},
                            "labels": [{"name": l} for l in body["labels"]], "body": body["body"]})
        self.assertEqual(q.expires_at - q.quarantined_at, dt.timedelta(days=fq.QUARANTINE_DAYS))


class ActiveCommandTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "test").mkdir()
        (self.root / "test/test_mixer.cpp").write_text(f'TEST_CASE("{FLAKY}") {{}}', encoding="utf-8")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_active(self, issues, scope=None, mode="enforce"):
        a = argparse.Namespace(repo="o/r", sha="abc", pr=None, source_root=str(self.root),
                               out_dir=str(self.root / "out"), mode=mode, bot_login=fq.BOT_LOGIN)
        scope = scope or fx.Scope(frozenset(), frozenset({"core/x.cpp"}))
        fq.cmd_active(a, issues_fn=lambda: issues, scope_fn=lambda: scope, now=NOW)
        return ((self.root / "out/names.txt").read_text(encoding="utf-8"), (self.root / "out/exclude.regex").read_text(encoding="utf-8"))

    def test_active_writes_names_and_an_escaped_anchored_fragment(self) -> None:
        names, regex = self.run_active([issue(7, FLAKY, 1), issue(8, "other.test", 1)])
        self.assertEqual(names.splitlines(), sorted([FLAKY, "other.test"]))
        self.assertEqual(regex, r"^Mixer meter settles under load \(x\|y\)$|^other\.test$")
        pattern = re.compile(regex)
        self.assertTrue(pattern.search(FLAKY))
        self.assertFalse(pattern.search("Mixer meter settles under load x"))
        self.assertFalse(pattern.search("otherXtest"))

    def test_a_touched_quarantined_test_stays_required(self) -> None:
        scope = fx.Scope(frozenset(), frozenset({"test/test_mixer.cpp"}))
        names, regex = self.run_active([issue(7, FLAKY, 1)], scope)
        self.assertEqual((names, regex), ("", ""))

    def test_unread_scope_keeps_everything_required(self) -> None:
        names, _ = self.run_active([issue(7, FLAKY, 1)], fx.Scope(frozenset(), frozenset(), "unreadable"))
        self.assertEqual(names, "")

    def test_unreadable_issues_quarantine_nothing(self) -> None:
        self.assertEqual(self.run_active(None), ("", ""))

    def test_kill_switch_quarantines_nothing(self) -> None:
        self.assertEqual(self.run_active([issue(7, FLAKY, 1)], mode="shadow"), ("", ""))

    def test_sweep_never_writes_in_shadow_mode(self) -> None:
        records = [rec(str(i), 1, FLAKY) for i in range(3)]
        a = argparse.Namespace(repo="o/r", dry_run=False, threshold=3, mode="shadow", bot_login=fq.BOT_LOGIN)
        with mock.patch.object(fq, "list_issues", return_value=[]), \
                mock.patch.object(fq, "load_records", return_value=(records, [])), \
                mock.patch.object(fq, "apply") as applied, mock.patch.object(fq, "ensure_labels"):
            self.assertEqual(fq.cmd_sweep(a, now=NOW), 0)
            applied.assert_not_called()
            a.mode = "enforce"
            applied.return_value = 0
            self.assertEqual(fq.cmd_sweep(a, now=NOW), 0)
            applied.assert_called_once()


class TrustTests(unittest.TestCase):
    def test_only_merge_group_build_runs_of_this_repo_are_trusted(self) -> None:
        art = {"workflow_run": {"id": 5}}
        good = {"id": 5, "path": fq.WORKFLOW_PATH, "event": "merge_group",
                "repository": {"full_name": "o/r"}, "head_repository": {"full_name": "o/r"}}
        self.assertIsNone(fq.trust_refusal("o/r", art, good))
        self.assertIsNotNone(fq.trust_refusal("o/r", art, dict(good, event="pull_request")))
        self.assertIsNotNone(fq.trust_refusal("o/r", art, dict(good, path=".github/workflows/x.yml")))
        self.assertIsNotNone(fq.trust_refusal("o/r", art, dict(good, head_repository={"full_name": "f/r"})))
        self.assertIsNotNone(fq.trust_refusal("o/r", art, dict(good, id=6)))
        self.assertIsNotNone(fq.trust_refusal("o/r", art, None))


if __name__ == "__main__":
    unittest.main()
