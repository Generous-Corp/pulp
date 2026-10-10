#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/ci/evidence_archive.py"


def run_collector(root: Path, data: dict, now: str, sessions: Path | None = None) -> dict:
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".json", delete=False) as fixture:
        json.dump(data, fixture)
        fixture_path = Path(fixture.name)
    try:
        command = [sys.executable, str(SCRIPT), "--root", str(root), "--github-json", str(fixture_path), "--now", now]
        if sessions is not None:
            command += ["--sessions-root", str(sessions)]
        result = subprocess.run(command, check=True, capture_output=True, text=True, encoding="utf-8")
        return json.loads(result.stdout)
    finally:
        fixture_path.unlink(missing_ok=True)


class EvidenceArchiveTests(unittest.TestCase):
    def test_redacts_and_is_idempotent_across_collection_days(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "archive"
            data = {"runs": [{"id": 7, "head_sha": "abc", "event": "push", "updated_at": "2026-10-09T12:00:00Z", "failure_evidence": "ghp_SECRET12345678 user@example.com api_key=live-key"}]}
            first = run_collector(root, data, "2026-10-10T00:00:00Z")
            second = run_collector(root, data, "2026-10-11T00:00:00Z")
            self.assertEqual(first["counts"]["ci_job"], 1)
            self.assertEqual(second["counts"]["skipped_duplicate"], 1)
            lines = "".join(path.read_text(encoding="utf-8") for path in root.glob("*.jsonl"))
            self.assertNotIn("ghp_SECRET", lines)
            self.assertNotIn("user@example.com", lines)
            self.assertNotIn("live-key", lines)
            self.assertIn("REDACTED", lines)
            self.assertEqual(sum(1 for line in lines.splitlines() if line.strip()), 1)

    def test_prune_removes_old_facts_and_keeps_fresh(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "archive"
            root.mkdir()
            facts = root / "facts-2026-10-10.jsonl"
            records = [
                {"record_id": "old", "observed_at": "2026-03-01T00:00:00Z", "kind": "ci_job"},
                {"record_id": "fresh", "observed_at": "2026-09-01T00:00:00Z", "kind": "ci_job"},
            ]
            facts.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")
            run_collector(root, {"runs": []}, "2026-10-10T00:00:00Z")
            kept = [json.loads(line) for line in facts.read_text(encoding="utf-8").splitlines()]
            self.assertEqual([record["record_id"] for record in kept], ["fresh"])

    def test_session_retention_is_shorter(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            sessions = Path(temporary) / "sessions"
            sessions.mkdir()
            (sessions / "x.jsonl").write_text(
                json.dumps({"timestamp": "2026-09-01T00:00:00Z", "error": "old"}) + "\n", encoding="utf-8"
            )
            result = run_collector(Path(temporary) / "archive", {"runs": []}, "2026-10-10T00:00:00Z", sessions)
            self.assertEqual(result["counts"]["skipped_retention"], 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
