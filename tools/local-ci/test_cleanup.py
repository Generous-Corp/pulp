#!/usr/bin/env python3
"""No-network tests for local-ci cleanup planning helpers."""

from __future__ import annotations

import importlib.util
import pathlib
import tempfile
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("cleanup.py")


def load_module():
    spec = importlib.util.spec_from_file_location("cleanup_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class CleanupTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
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

    def test_result_file_job_id_extracts_stable_component(self) -> None:
        self.assertEqual(
            self.mod.result_file_job_id(pathlib.Path("20260404-120000-job123-feature.json")),
            "job123",
        )
        self.assertIsNone(self.mod.result_file_job_id(pathlib.Path("not-json.txt")))
        self.assertIsNone(self.mod.result_file_job_id(pathlib.Path("too-short.json")))

    def test_cleanup_plan_retains_live_and_queue_artifacts(self) -> None:
        queue = [
            {"id": "live", "status": "running"},
            {"id": "retained", "status": "completed"},
        ]
        for job_id in ("live", "retained", "old"):
            (self.bundles / f"{job_id}.bundle").write_text(job_id)
            (self.logs / job_id).mkdir()
            (self.logs / job_id / "mac.log").write_text(job_id)
            (self.results / f"result-full-{job_id}.json").write_text("{}")
        prepared_full = self.prepared / "mac" / "full"
        prepared_full.mkdir(parents=True)
        (prepared_full / "stamp").write_text("ok")

        plan = self.collect(
            queue,
            keep_results=0,
            keep_logs=0,
            keep_bundles=0,
            include_prepared=True,
        )

        self.assertEqual(
            {pathlib.Path(entry["path"]).name for entry in plan["categories"]["bundles"]},
            {"old.bundle", "retained.bundle"},
        )
        self.assertEqual(
            {pathlib.Path(entry["path"]).name for entry in plan["categories"]["logs"]},
            {"old"},
        )
        self.assertEqual(
            {pathlib.Path(entry["path"]).name for entry in plan["categories"]["results"]},
            {"result-full-old.json"},
        )
        self.assertEqual([entry["path"] for entry in plan["categories"]["prepared"]], [prepared_full])
        self.assertEqual(plan["total_paths"], 5)

    def test_apply_cleanup_plan_removes_files_and_directories(self) -> None:
        bundle = self.bundles / "old.bundle"
        bundle.write_text("old")
        log_dir = self.logs / "old"
        log_dir.mkdir()
        (log_dir / "mac.log").write_text("old")
        plan = {
            "categories": {
                "bundles": [{"path": bundle, "size_bytes": 3}],
                "logs": [{"path": log_dir, "size_bytes": 3}],
            }
        }

        result = self.mod.apply_local_ci_cleanup_plan(plan)

        self.assertFalse(bundle.exists())
        self.assertFalse(log_dir.exists())
        self.assertEqual(len(result["removed"]), 2)
        self.assertEqual(result["removed_bytes"], 6)
        self.assertEqual(result["failed"], [])


if __name__ == "__main__":
    unittest.main()
