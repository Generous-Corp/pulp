#!/usr/bin/env python3
"""Focused controls for the graph-agent stall guard."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import graph_stall_guard as guard  # noqa: E402


NOW = dt.datetime(2026, 11, 8, 10, 0, tzinfo=dt.timezone.utc)
NOW_TEXT = "2026-11-08T10:00:00Z"
HEAD = "0123456789abcdef0123456789abcdef01234567"


def manifest(lanes):
    return {
        "schema": guard.SCHEMA,
        "generated_at": NOW_TEXT,
        "lanes": list(lanes),
    }


def lane(*, status="PARTIAL", receipt_path="receipt.json", receipt_sha256=None, **extra):
    value = {
        "lane": "G-7",
        "status": status,
        "heartbeat_at": "2026-11-08T09:45:00Z",
        "receipt_path": receipt_path,
        "receipt_sha256": receipt_sha256,
        "pr_required": False,
        **extra,
    }
    return value


class GuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.receipt = self.root / "receipt.json"
        self.receipt.write_text('{"result":"pass"}\n', encoding="utf-8")
        self.digest = hashlib.sha256(self.receipt.read_bytes()).hexdigest()

    def tearDown(self):
        self.temp.cleanup()

    def run_guard(self, value, **kwargs):
        return guard.analyze(value, root=self.root, now=NOW, **kwargs)

    def test_current_heartbeat_receipt_and_optional_pr_are_pass(self):
        result = self.run_guard(
            manifest(
                [
                    lane(
                        receipt_sha256=self.digest,
                        pr_required=True,
                        pr={
                            "number": 11700,
                            "url": "https://github.com/Generous-Corp/pulp/pull/11700",
                            "head": HEAD,
                            "base": "main",
                        },
                    )
                ]
            )
        )
        self.assertEqual(result["verdict"], "PASS")
        self.assertEqual(result["lanes"][0]["status"], "PARTIAL")

    def test_stale_heartbeat_becomes_actionable_blocked(self):
        value = manifest([lane(receipt_sha256=self.digest, next_action="refresh G-7 receipt")])
        value["lanes"][0]["heartbeat_at"] = "2026-11-08T07:00:00Z"
        value["lanes"][0]["blocker"] = "owner heartbeat missing"
        result = self.run_guard(value, max_heartbeat_minutes=90)
        self.assertEqual(result["verdict"], "BLOCKED")
        self.assertEqual(result["lanes"][0]["status"], "BLOCKED")
        self.assertTrue(
            any(reason.startswith("heartbeat is 180.0 minutes old") for reason in result["lanes"][0]["reasons"])
        )
        self.assertEqual(result["lanes"][0]["next_action"], "refresh G-7 receipt")

    def test_receipt_mutation_is_blocked(self):
        value = manifest([lane(receipt_sha256=self.digest)])
        self.receipt.write_text('{"result":"mutated"}\n', encoding="utf-8")
        result = self.run_guard(value)
        self.assertEqual(result["verdict"], "BLOCKED")
        self.assertIn("receipt SHA-256 does not match receipt_sha256", result["lanes"][0]["reasons"])

    def test_missing_receipt_is_blocked(self):
        result = self.run_guard(manifest([lane(receipt_path="missing.json", receipt_sha256="0" * 64)]))
        self.assertEqual(result["verdict"], "BLOCKED")
        self.assertIn("receipt does not exist: missing.json", result["lanes"][0]["reasons"])

    def test_receipt_symlink_cannot_escape_manifest_directory(self):
        outside = self.root.parent / "outside-receipt.json"
        outside.write_text('{"result":"outside"}\n', encoding="utf-8")
        try:
            (self.root / "escaped.json").symlink_to(outside)
            result = self.run_guard(
                manifest([lane(receipt_path="escaped.json", receipt_sha256="0" * 64)])
            )
            self.assertEqual(result["verdict"], "BLOCKED")
            self.assertIn(
                "receipt_path must stay below the manifest directory",
                result["lanes"][0]["reasons"],
            )
        finally:
            outside.unlink(missing_ok=True)

    def test_pr_linkage_requires_complete_canonical_head(self):
        value = manifest(
            [
                lane(
                    receipt_sha256=self.digest,
                    pr_required=True,
                    pr={
                        "number": 11700,
                        "url": "https://github.com/Generous-Corp/pulp/pull/11700",
                        "head": HEAD[:-1],
                        "base": "main",
                    },
                )
            ]
        )
        result = self.run_guard(value)
        self.assertEqual(result["verdict"], "BLOCKED")
        self.assertIn("PR head must be the complete 40-hex commit SHA", result["lanes"][0]["reasons"])

    def test_pr_required_must_be_boolean(self):
        value = manifest([lane(receipt_sha256=self.digest, pr_required="false")])
        result = self.run_guard(value)
        self.assertEqual(result["verdict"], "BLOCKED")
        self.assertIn("pr_required must be boolean", result["lanes"][0]["reasons"])

    def test_blocked_status_requires_recovery_action(self):
        result = self.run_guard(
            manifest([{"lane": "G-9", "status": "BLOCKED", "blocker": "SDK unavailable"}])
        )
        self.assertEqual(result["verdict"], "BLOCKED")
        self.assertIn("BLOCKED status requires next_action", result["lanes"][0]["reasons"])

    def test_old_manifest_is_blocked_even_with_fresh_lane(self):
        value = manifest([lane(receipt_sha256=self.digest)])
        value["generated_at"] = "2026-11-08T05:00:00Z"
        result = self.run_guard(value, max_snapshot_minutes=180)
        self.assertEqual(result["verdict"], "BLOCKED")
        self.assertTrue(any(reason.startswith("manifest is 300.0 minutes old") for reason in result["reasons"]))

    def test_duplicate_lane_names_are_blocked(self):
        result = self.run_guard(
            manifest(
                [
                    lane(receipt_sha256=self.digest),
                    lane(receipt_sha256=self.digest),
                ]
            )
        )
        self.assertEqual(result["verdict"], "BLOCKED")
        self.assertIn("duplicate lane names: G-7", result["reasons"])

    def test_cli_emits_actionable_blocked_line_and_nonzero(self):
        path = self.root / "manifest.json"
        path.write_text(json.dumps(manifest([lane(receipt_path="missing.json", receipt_sha256="0" * 64)])), encoding="utf-8")
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("graph_stall_guard.py")),
                str(path),
                "--now",
                NOW_TEXT,
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
        )
        self.assertEqual(proc.returncode, 1)
        self.assertIn("BLOCKED G-7:", proc.stderr)
        self.assertIn("receipt does not exist", proc.stderr)


if __name__ == "__main__":
    unittest.main()
