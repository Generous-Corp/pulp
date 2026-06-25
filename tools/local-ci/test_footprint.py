#!/usr/bin/env python3
"""Tests for the footprint size/footprint helpers."""

import os
import tempfile
import unittest
from unittest import mock
from pathlib import Path

import footprint



class FootprintTests(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        root = Path(self.tmpdir.name)
        self.state_dir = root / "state"

        self.prev_home = os.environ.get("PULP_LOCAL_CI_HOME")
        os.environ["PULP_LOCAL_CI_HOME"] = str(self.state_dir)
        self.mod = footprint

    def tearDown(self):
        if self.prev_home is None:
            os.environ.pop("PULP_LOCAL_CI_HOME", None)
        else:
            os.environ["PULP_LOCAL_CI_HOME"] = self.prev_home

        self.tmpdir.cleanup()


    def test_size_helpers_report_files_and_local_ci_state_footprint(self):
        self.assertEqual(self.mod.format_size_bytes(None), "")
        self.assertEqual(self.mod.format_size_bytes(""), "")
        self.assertEqual(self.mod.format_size_bytes(512), "512 B")
        self.assertEqual(self.mod.format_size_bytes(1536), "1.5 KB")
        self.assertEqual(self.mod.format_size_bytes(1024 * 1024), "1.0 MB")
        self.assertEqual(self.mod.format_size_bytes(1024 * 1024 * 1024), "1.0 GB")
        self.assertEqual(self.mod.format_size_bytes(1024 * 1024 * 1024 * 1024), "1.0 TB")
        self.assertEqual(self.mod.path_size_bytes(self.state_dir / "missing"), 0)

        bundle_dir = self.state_dir / "bundles"
        bundle_dir.mkdir(parents=True)
        (bundle_dir / "a.bundle").write_bytes(b"abc")
        nested = bundle_dir / "nested"
        nested.mkdir()
        (nested / "b.bundle").write_bytes(b"defg")

        self.assertEqual(self.mod.path_size_bytes(bundle_dir), 7)
        footprint = self.mod.local_ci_state_footprint()
        self.assertEqual(footprint["entries"]["bundles"]["size_bytes"], 7)
        self.assertEqual(footprint["total_bytes"], 7)
        self.assertEqual(
            self.mod.state_footprint_lines(footprint, indent="  "),
            [
                "  Local CI footprint: total=7 B",
                "    bundles: 7 B (bundles)",
                "    prepared: 0 B (prepared)",
                "    logs: 0 B (logs)",
                "    results: 0 B (results)",
                "    cloud-runs: 0 B (cloud-runs)",
            ],
        )
        self.assertEqual(self.mod.describe_path_for_cleanup(bundle_dir), "bundles")
        outside = Path(self.tmpdir.name) / "outside"
        self.assertEqual(self.mod.describe_path_for_cleanup(outside), str(outside))

    def test_path_size_handles_files_and_stat_errors(self):
        file_path = self.state_dir / "payload.bin"
        file_path.parent.mkdir(parents=True)
        file_path.write_bytes(b"abcd")
        self.assertEqual(self.mod.path_size_bytes(file_path), 4)

        with mock.patch.object(Path, "exists", side_effect=OSError("boom")):
            self.assertEqual(self.mod.path_size_bytes(file_path), 0)

        walk_root = self.state_dir / "walk"
        walk_root.mkdir()
        good = walk_root / "good.bin"
        bad = walk_root / "bad.bin"
        good.write_bytes(b"123")
        bad.write_bytes(b"4567")
        original_stat = Path.stat

        def flaky_stat(path, *args, **kwargs):
            if path == bad:
                raise OSError("stat failed")
            return original_stat(path, *args, **kwargs)

        with mock.patch.object(Path, "stat", flaky_stat):
            self.assertEqual(self.mod.path_size_bytes(walk_root), 3)

    def test_describe_path_for_cleanup_relativizes_nested_state_paths(self):
        inside = self.state_dir / "logs" / "job" / "mac.log"
        outside = Path(self.tmpdir.name) / "elsewhere.log"

        self.assertEqual(self.mod.describe_path_for_cleanup(inside), "logs/job/mac.log")
        self.assertEqual(self.mod.describe_path_for_cleanup(outside), str(outside))



if __name__ == "__main__":
    unittest.main()
