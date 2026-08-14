import json
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock

import shipyard_steward_outbox as outbox


NOW = datetime(2026, 8, 14, 12, 0, tzinfo=timezone.utc)


def report(*prs, errors=()):
    return {
        "repos": [
            {
                "repo": "Generous-Corp/pulp",
                "base": "main",
                "prs": list(prs),
                "errors": list(errors),
            }
        ]
    }


def pr(number, action, **decision):
    return {
        "number": number,
        "head_sha": f"{number:040d}",
        "decision": {"action": action, **decision},
        "error": None,
    }


def pull(number, title="Useful title", labels=()):
    return {
        "number": number,
        "title": title,
        "created_at": "2026-08-06T12:00:00Z",
        "updated_at": "2026-08-13T12:00:00Z",
        "user": {"login": "agent"},
        "labels": [{"name": label} for label in labels],
        "base": {"ref": "main"},
        "head": {"sha": f"{number:040d}"},
    }


class OutboxTests(unittest.TestCase):
    def test_keeps_only_exceptions_and_explains_unmanaged(self):
        rows, errors, repo = outbox.build_outbox(
            report(
                pr(1, "waiting_required", contexts=["macos"]),
                pr(2, "unmanaged"),
                pr(3, "required_failed", contexts=["linux", "macos"]),
            ),
            [pull(1), pull(2, labels=["1·codex"]), pull(3)],
            NOW,
        )
        self.assertEqual(repo, "Generous-Corp/pulp")
        self.assertEqual(errors, [])
        self.assertEqual([row.number for row in rows], [2, 3])
        self.assertEqual(rows[0].reason, "no explicit exact-head Shipyard handoff")
        self.assertIn("linux, macos", rows[1].reason)

    def test_report_error_overrides_normal_decision(self):
        broken = pr(4, "queued")
        broken["error"] = "mutation failed"
        rows, errors, _ = outbox.build_outbox(report(broken), [pull(4)], NOW)
        self.assertEqual(errors, [])
        self.assertEqual(rows[0].action, "mutation_error")

    def test_paginated_pull_shape_and_hostile_title_are_safe(self):
        rows, _, repo = outbox.build_outbox(
            report(pr(5, "unmanaged")),
            [[pull(5, title="hey ](bad) @team\nnext", labels=["2·m5"])]],
            NOW,
        )
        body = outbox.render_markdown(rows, [], repo, NOW)
        self.assertIn("\\]", body)
        self.assertIn("@\u200bteam", body)
        self.assertNotIn("@team", body)
        self.assertEqual(body.count("\nnext"), 0)

    def test_empty_outbox_is_closeable(self):
        rows, errors, repo = outbox.build_outbox(
            report(pr(6, "queued")), [pull(6)], NOW
        )
        body = outbox.render_markdown(rows, errors, repo, NOW)
        self.assertEqual(rows, [])
        self.assertIn("exception count reaches zero", body)

    def test_open_pr_outside_steward_base_is_never_dropped(self):
        outside = pull(8, title="Old stacked work")
        outside["base"] = {"ref": "hold/generated-sequence"}
        rows, errors, repo = outbox.build_outbox(
            report(pr(6, "queued")), [pull(6), outside], NOW
        )
        self.assertEqual(errors, [])
        self.assertEqual([row.number for row in rows], [8])
        self.assertEqual(rows[0].action, "outside_scope")
        self.assertIn("hold/generated-sequence", rows[0].reason)
        body = outbox.render_markdown(rows, errors, repo, NOW)
        self.assertIn("Retarget into a managed landing chain", body)

    def test_new_managed_base_pr_between_snapshots_is_a_visible_census_gap(self):
        rows, errors, repo = outbox.build_outbox(
            report(pr(6, "queued")), [pull(6), pull(9, title="Just opened")], NOW
        )
        self.assertEqual(errors, [])
        self.assertEqual([row.number for row in rows], [9])
        self.assertEqual(rows[0].action, "census_gap")
        body = outbox.render_markdown(rows, errors, repo, NOW)
        self.assertIn("next tick must observe it", body)

    def test_cli_writes_stable_state(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            report_path = root / "report.json"
            pulls_path = root / "pulls.json"
            body_path = root / "body.md"
            state_path = root / "state.json"
            report_path.write_text(json.dumps(report(pr(7, "unmanaged"))))
            pulls_path.write_text(json.dumps([pull(7)]))
            argv = [
                "shipyard_steward_outbox.py",
                "--report", str(report_path),
                "--pulls", str(pulls_path),
                "--body", str(body_path),
                "--state", str(state_path),
                "--now", "2026-08-14T12:00:00Z",
            ]
            with mock.patch.object(sys, "argv", argv):
                self.assertEqual(outbox.main(), 0)
            self.assertEqual(json.loads(state_path.read_text())["exception_count"], 1)
            self.assertIn("#7 Useful title", body_path.read_text())


if __name__ == "__main__":
    unittest.main()
