#!/usr/bin/env python3
"""Tests for tools/ci/lane_reuse_record.py.

What must hold:
- without SHIPYARD_REUSE_RECORD_DIR the lane's ctest runs exactly as given and
  nothing is recorded;
- with it, ctest also writes JUnit into the record, ctest's LastTest.log is
  kept, and reuse_record.py is called as a lane run named by the directory,
  with build.yml's flags and a `full` suite carrying attempts and `repeat`;
- the test stage exits with ctest's status whatever the recorder does;
- a bounded plan's legs are copied out of the runner's private directory, and
  a leg that wrote no report is left out.

Run:
    python3 tools/ci/test_lane_reuse_record.py
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import lane_reuse_record as lrr  # noqa: E402

# Logs its argv, writes a JUnit report where --output-junit says and
# ctest's LastTest.log, and exits with FAKE_CTEST_RC.
FAKE_CTEST = """import json, os, sys
from pathlib import Path
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(json.dumps({"ctest": sys.argv[1:]}) + "\\n")
if "--output-junit" in sys.argv:
    Path(sys.argv[sys.argv.index("--output-junit") + 1]).write_text("<testsuite/>")
log = Path(os.environ["FAKE_BUILD"]) / "Testing" / "Temporary" / "LastTest.log"
log.parent.mkdir(parents=True, exist_ok=True)
log.write_text("attempts")
sys.exit(int(os.environ.get("FAKE_CTEST_RC", "0")))
"""
# Logs its argv, writes job.json into --out-dir, exits with FAKE_RECORDER_RC.
FAKE_RECORDER = """import json, os, sys
from pathlib import Path
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(json.dumps({"recorder": sys.argv[1:]}) + "\\n")
out = Path(sys.argv[sys.argv.index("--out-dir") + 1])
(out / "job.json").write_text("{}")
sys.exit(int(os.environ.get("FAKE_RECORDER_RC", "0")))
"""


class LaneReuseRecordTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.log = self.root / "calls.jsonl"
        (self.root / "ctest.py").write_text(FAKE_CTEST)
        (self.root / "recorder.py").write_text(FAKE_RECORDER)
        self.out = self.root / "pending" / "abc123-77-42"
        self.out.mkdir(parents=True)
        self.env = {"FAKE_LOG": str(self.log), "FAKE_BUILD": str(self.build)}
        patcher = mock.patch.object(lrr, "RECORDER", self.root / "recorder.py")
        patcher.start()
        self.addCleanup(patcher.stop)

    def calls(self) -> list[dict]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def run_stage(self, extra_env: dict[str, str]) -> int:
        ctest = [sys.executable, str(self.root / "ctest.py"), "--test-dir", str(self.build), "--repeat", "until-pass:2"]
        env = {k: v for k, v in os.environ.items() if k != lrr.RECORD_DIR_ENV}
        env.update(self.env, **extra_env)
        with mock.patch.dict(os.environ, env, clear=True):
            return lrr.main(["lane_reuse_record.py", "test", "--build-dir", str(self.build), "--", *ctest])

    def test_without_the_directory_ctest_runs_as_given_and_nothing_is_recorded(self) -> None:
        self.assertEqual(self.run_stage({"FAKE_CTEST_RC": "3"}), 3)
        calls = self.calls()
        self.assertEqual(len(calls), 1, calls)
        self.assertNotIn("--output-junit", calls[0]["ctest"])
        self.assertFalse((self.out / "job.json").exists())

    def test_with_the_directory_the_run_is_recorded_as_a_lane_run(self) -> None:
        rc = self.run_stage({lrr.RECORD_DIR_ENV: str(self.out), "FAKE_CTEST_RC": "8"})
        self.assertEqual(rc, 8, "the stage's verdict is ctest's")
        ctest, recorder = self.calls()
        junit = self.out / "suites" / "full" / "ctest.junit.xml"
        self.assertEqual(ctest["ctest"][-2:], ["--output-junit", str(junit)])
        self.assertTrue(junit.is_file())
        argv = recorder["recorder"]
        self.assertEqual(argv[:5], ["write", "--run-kind", "lane", "--run-id", "abc123-77-42"])
        for flag in ("--link-members", "--object-deps", "--codemodel", "--inventory"):
            self.assertIn(flag, argv)
        self.assertEqual(argv[argv.index("--out-dir") + 1], str(self.out))
        attempts = self.out / "suites" / "full" / "LastTest.log"
        self.assertEqual(argv[argv.index("--suite") + 1], f"full={junit},attempts={attempts},repeat")
        self.assertEqual(attempts.read_text(), "attempts")

    def test_a_recorder_failure_never_changes_the_verdict(self) -> None:
        rc = self.run_stage({lrr.RECORD_DIR_ENV: str(self.out), "FAKE_RECORDER_RC": "1"})
        self.assertEqual(rc, 0)
        self.assertEqual(len(self.calls()), 2)

    def test_a_bounded_plans_legs_are_copied_out_and_a_silent_leg_is_left_out(self) -> None:
        private = self.root / "private"
        private.mkdir()
        (private / "selected-junit.xml").write_text("<testsuite name='selected'/>")
        env = dict(self.env, **{lrr.RECORD_DIR_ENV: str(self.out)})
        with mock.patch.dict(os.environ, env):
            rc = lrr.record_legs(self.build, self.root,
                                 [("pr-affected", private / "selected-junit.xml", None),
                                  ("full", private / "full-junit.xml", None)], "success")
        self.assertEqual(rc, 0)
        copied = self.out / "suites" / "pr-affected" / "ctest.junit.xml"
        self.assertEqual(copied.read_text(), "<testsuite name='selected'/>")
        argv = self.calls()[0]["recorder"]
        suites = [argv[i + 1] for i, a in enumerate(argv) if a == "--suite"]
        self.assertEqual(suites, [f"pr-affected={copied},repeat"])

    def test_without_the_directory_a_bounded_plan_records_nothing(self) -> None:
        env = {k: v for k, v in os.environ.items() if k != lrr.RECORD_DIR_ENV}
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertEqual(lrr.record_legs(self.build, self.root, [], "success"), 0)
        self.assertEqual(self.calls(), [])


if __name__ == "__main__":
    unittest.main()
