#!/usr/bin/env python3
"""Tests for split desktop publish report listing helpers."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("reporting_publish_list.py")


class ReportingPublishListTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.config = {"desktop_automation": {"artifact_root": str(self.root / "artifacts")}}

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def publish_root(self, _config: dict) -> Path:
        path = self.root / "published"
        path.mkdir(parents=True, exist_ok=True)
        return path

    def test_publish_reports_skip_bad_indexes_and_write_rollups(self) -> None:
        old_dir = self.publish_root(self.config) / "old"
        new_dir = self.publish_root(self.config) / "new"
        bad_dir = self.publish_root(self.config) / "bad"
        for path in (old_dir, new_dir, bad_dir):
            path.mkdir(parents=True)
        (old_dir / "index.json").write_text(json.dumps({"generated_at": "2026-01-01T00:00:00Z", "label": "old"}))
        (new_dir / "index.json").write_text(json.dumps({"generated_at": "2026-01-02T00:00:00Z", "label": "new"}))
        (bad_dir / "index.json").write_text("{")

        writes: dict[Path, str] = {}
        reports = self.mod.desktop_publish_reports(self.config, desktop_publish_root_fn=self.publish_root)
        self.mod.write_desktop_publish_rollups(
            self.config,
            desktop_publish_root_fn=self.publish_root,
            desktop_publish_reports_fn=lambda _config: reports,
            atomic_write_text_fn=lambda path, text: writes.__setitem__(path, text),
        )

        self.assertEqual([report["label"] for report in reports], ["new", "old"])
        self.assertEqual(json.loads(writes[self.publish_root(self.config) / "latest-report.json"])["label"], "new")
        self.assertEqual(len(writes[self.publish_root(self.config) / "reports.jsonl"].splitlines()), 2)


if __name__ == "__main__":
    unittest.main()
