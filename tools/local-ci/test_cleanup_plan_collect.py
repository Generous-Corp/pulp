#!/usr/bin/env python3
"""Tests for cleanup artifact plan collection."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cleanup_plan_collect.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_cleanup_plan_collect", add_module_dir=True)


class CleanupPlanCollectTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.bundles = self.root / "bundles"
        self.logs = self.root / "logs"
        self.results = self.root / "results"
        self.prepared = self.root / "prepared"
        for path in (self.bundles, self.logs, self.results, self.prepared):
            path.mkdir(parents=True, exist_ok=True)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def collect(self, queue, **kwargs):
        return self.mod.collect_local_ci_cleanup_plan(
            queue,
            bundles_dir_fn=lambda: self.bundles,
            logs_dir_fn=lambda: self.logs,
            results_dir_fn=lambda: self.results,
            prepared_dir_fn=lambda: self.prepared,
            path_size_bytes_fn=lambda path: sum(file.stat().st_size for file in path.rglob("*") if file.is_file()),
            **kwargs,
        )

    def test_cleanup_plan_retains_live_and_queue_artifacts(self) -> None:
        queue = [{"id": "live", "status": "running"}, {"id": "retained", "status": "completed"}]
        for job_id in ("live", "retained", "old"):
            (self.bundles / f"{job_id}.bundle").write_text(job_id)
            (self.logs / job_id).mkdir()
            (self.logs / job_id / "mac.log").write_text(job_id)
            (self.results / f"result-full-{job_id}.json").write_text("{}")
        prepared_full = self.prepared / "mac" / "full"
        prepared_full.mkdir(parents=True)
        (prepared_full / "stamp").write_text("ok")

        plan = self.collect(queue, keep_results=0, keep_logs=0, keep_bundles=0, include_prepared=True)

        self.assertEqual({Path(entry["path"]).name for entry in plan["categories"]["bundles"]}, {"old.bundle", "retained.bundle"})
        self.assertEqual({Path(entry["path"]).name for entry in plan["categories"]["logs"]}, {"old"})
        self.assertEqual({Path(entry["path"]).name for entry in plan["categories"]["results"]}, {"result-full-old.json"})
        self.assertEqual([entry["path"] for entry in plan["categories"]["prepared"]], [prepared_full])
        self.assertEqual(plan["total_paths"], 5)


if __name__ == "__main__":
    unittest.main()
