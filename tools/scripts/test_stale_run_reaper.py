#!/usr/bin/env python3
"""Tests for stale_run_reaper.py: escalation, phantoms, and the live-head guard.

A fake API stands in for GitHub and answers cancel / force-cancel exactly the
way the real endpoints answered on 2026-10-02 for runs that had sat `queued`
for weeks.
"""

from __future__ import annotations

import datetime as dt
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import stale_run_reaper as reaper  # noqa: E402

NOW = dt.datetime(2026, 10, 2, 12, 0, tzinfo=dt.timezone.utc).timestamp()
ROOT = pathlib.Path(__file__).resolve().parents[2]


def stamp(minutes_ago: int) -> str:
    moment = dt.datetime.fromtimestamp(NOW - minutes_ago * 60, dt.timezone.utc)
    return moment.strftime("%Y-%m-%dT%H:%M:%SZ")


def run(run_id: int, *, minutes: int, name: str = "Docs Consistency",
        head: str = "a" * 40, status: str = "queued", **extra) -> dict:
    value = {
        "id": run_id, "name": name, "workflow_id": 1000 + run_id,
        "path": f".github/workflows/{name.lower().replace(' ', '-')}.yml",
        "status": status, "created_at": stamp(minutes), "run_started_at": None,
        "head_branch": "feature/x", "head_sha": head,
    }
    value.update(extra)
    return value


class FakeApi:
    def __init__(self, runs: dict[str, list[dict]], *, open_heads=(),
                 cancel=None, force=None):
        self.repo = "Generous-Corp/pulp"
        self.calls = 0
        self.runs = runs
        self.open_heads = list(open_heads)
        self.cancel = cancel or {}
        self.force = force or {}
        self.posts: list[str] = []

    def paged(self, path: str, key: str):
        self.calls += 1
        if "/pulls" in path:
            return [{"head": {"sha": sha}} for sha in self.open_heads]
        status = path.split("status=")[1]
        return self.runs.get(status, [])

    def post(self, path: str):
        self.calls += 1
        self.posts.append(path)
        run_id = int(path.split("/runs/")[1].split("/")[0])
        table = self.force if path.endswith("/force-cancel") else self.cancel
        answer = table.get(run_id)
        if answer is not None:
            raise reaper.ApiError(*answer)
        return {}


NOT_QUEUED = (409, "Cannot cancel a workflow run that has not been queued yet.")


class ReaperTest(unittest.TestCase):
    def reap(self, api, **kwargs):
        lines: list[str] = []
        report = reaper.reap(api, now=NOW, self_id=kwargs.pop("self_id", 1),
                             in_progress_max=240, queued_max=480,
                             log=lines.append, **kwargs)
        return report, lines

    def test_a_fresh_queued_run_is_left_alone(self):
        api = FakeApi({"queued": [run(10, minutes=60)]})
        report, _ = self.reap(api)
        self.assertEqual(report.outcomes, [])
        self.assertEqual(api.posts, [])

    def test_a_stale_run_gets_a_plain_cancel_first(self):
        api = FakeApi({"queued": [run(10, minutes=600)]})
        report, _ = self.reap(api)
        self.assertEqual([o.result for o in report.outcomes], ["cancel-requested"])
        self.assertEqual(api.posts, ["/repos/Generous-Corp/pulp/actions/runs/10/cancel"])

    def test_a_refused_cancel_escalates_to_force_cancel(self):
        api = FakeApi({"queued": [run(10, minutes=600)]}, cancel={10: NOT_QUEUED})
        report, _ = self.reap(api)
        self.assertEqual([o.result for o in report.outcomes], ["force-cancel-requested"])
        self.assertTrue(api.posts[-1].endswith("/runs/10/force-cancel"))

    def test_a_run_refusing_both_is_a_phantom_not_a_cancel(self):
        api = FakeApi({"queued": [run(10, minutes=60000)]},
                      cancel={10: NOT_QUEUED}, force={10: NOT_QUEUED})
        report, _ = self.reap(api)
        self.assertEqual([o.result for o in report.outcomes], ["phantom"])
        text = reaper.summary(report, in_progress_max=240, queued_max=480, calls=api.calls)
        self.assertIn("Cancels requested: **0**", text)
        self.assertIn("GitHub-side phantoms", text)
        self.assertNotIn("CI queue is clean", text)

    def test_a_live_pr_head_is_never_force_cancelled(self):
        head = "b" * 40
        api = FakeApi({"queued": [run(10, minutes=600, head=head)]},
                      open_heads=[head], cancel={10: NOT_QUEUED})
        report, _ = self.reap(api)
        self.assertEqual([o.result for o in report.outcomes], ["live-head"])
        self.assertFalse(any(p.endswith("force-cancel") for p in api.posts))

    def test_a_non_409_failure_is_reported_without_escalation(self):
        api = FakeApi({"queued": [run(10, minutes=600)]}, cancel={10: (500, "boom")})
        report, _ = self.reap(api)
        self.assertEqual([o.result for o in report.outcomes], ["failed"])
        self.assertEqual(len(api.posts), 1)

    def test_build_and_test_is_reserved_for_typed_recovery(self):
        build = run(10, minutes=60000, name="Build and Test", workflow_id=256999733,
                    path=".github/workflows/build.yml")
        api = FakeApi({"queued": [build]})
        report, _ = self.reap(api)
        self.assertEqual(report.outcomes, [])
        self.assertEqual(api.posts, [])

    def test_build_workflow_identity_drift_refuses_the_sweep(self):
        drifted = run(10, minutes=60000, name="Build and Test", workflow_id=1,
                      path=".github/workflows/build.yml")
        with self.assertRaises(SystemExit):
            self.reap(FakeApi({"queued": [drifted]}))

    def test_in_progress_is_aged_from_execution_start(self):
        started_recently = run(10, minutes=600, status="in_progress",
                               run_started_at=stamp(30))
        api = FakeApi({"in_progress": [started_recently]})
        report, _ = self.reap(api)
        self.assertEqual(report.outcomes, [])

    def test_its_own_run_is_never_cancelled(self):
        api = FakeApi({"in_progress": [run(7, minutes=6000, status="in_progress",
                                           run_started_at=stamp(6000))]})
        report, _ = self.reap(api, self_id=7)
        self.assertEqual(report.outcomes, [])

    def test_the_workflow_runs_this_script(self):
        text = (ROOT / ".github/workflows/stale-run-reaper.yml").read_text()
        self.assertIn("python3 tools/scripts/stale_run_reaper.py", text)
        self.assertIn("actions: write", text)


if __name__ == "__main__":
    unittest.main()
