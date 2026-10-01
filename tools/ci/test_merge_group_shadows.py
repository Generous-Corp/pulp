#!/usr/bin/env python3
"""Tests for tools/ci/merge_group_shadows.py (the one merge-group shadow step).

What must hold:
- the plan runs binary identity always, affected tests when ctest ran and the
  merge commit has a first parent, flake exoneration only on a failed ctest
  outcome, and per-test receipts whenever ctest ran (passed or failed), with
  its receipts file and binary identity's hashes in the work dir; each
  instrument gets the arguments it documents;
- an instrument that raises or exits non-zero is named in the summary and
  never fails the runner (exit 0 in every case);
- the CLI prints one summary line naming every instrument's status.

Run:
    python3 tools/ci/test_merge_group_shadows.py
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import merge_group_shadows as mgs  # noqa: E402


def args(**kw) -> argparse.Namespace:
    base = dict(build_dir="/b", source_root="/s", repository="O/R", merge_sha="m", token="t",
                junit="/b/ctest.junit.xml", selected_json="/t/selected.json", ctest_outcome="success",
                hours=24, work_dir="/t", run_id="55")
    base.update(kw)
    return argparse.Namespace(**base)


class PlanTests(unittest.TestCase):
    def test_success_runs_identity_and_affected_but_not_exoneration(self) -> None:
        with mock.patch.object(mgs, "_first_parent", return_value="p1"):
            steps = {label: argv for label, _mod, argv in mgs.plan(args())}
        self.assertIn("--merge-sha", steps["binary-identity"])
        self.assertEqual(steps["affected-tests"][steps["affected-tests"].index("--base") + 1], "p1")
        self.assertIsNone(steps["flake-exoneration"])

    def test_failure_adds_exoneration_with_the_window(self) -> None:
        with mock.patch.object(mgs, "_first_parent", return_value="p1"):
            steps = {label: argv for label, _mod, argv in mgs.plan(args(ctest_outcome="failure", hours=36))}
        self.assertEqual(steps["flake-exoneration"][steps["flake-exoneration"].index("--hours") + 1], "36")
        self.assertIsNotNone(steps["affected-tests"])

    def test_receipts_run_when_ctest_ran_and_write_into_the_work_dir(self) -> None:
        for outcome in ("success", "failure"):
            with mock.patch.object(mgs, "_first_parent", return_value="p1"):
                steps = {label: argv for label, _mod, argv in mgs.plan(args(ctest_outcome=outcome))}
            argv = steps["test-receipts"]
            self.assertEqual(argv[0], "run")
            self.assertEqual(argv[argv.index("--receipts-out") + 1], "/t/test-receipts.json")
            self.assertEqual(argv[argv.index("--identity-json") + 1], "/t/our-identity.json")
            self.assertEqual(argv[argv.index("--keys-out") + 1], "/t/test-keys.json")
            self.assertEqual(argv[argv.index("--run-id") + 1], "55")
        for outcome in ("skipped", "cancelled"):
            with mock.patch.object(mgs, "_first_parent", return_value="p1"):
                steps = {label: argv for label, _mod, argv in mgs.plan(args(ctest_outcome=outcome))}
            self.assertIsNone(steps["test-receipts"], outcome)
        labels = [label for label, _m, _a in mgs.plan(args())]
        self.assertLess(labels.index("binary-identity"), labels.index("test-receipts"),
                        "identity must run first so receipts can reuse its hashes")

    def test_skipped_ctest_or_no_parent_skips_affected_tests(self) -> None:
        with mock.patch.object(mgs, "_first_parent", return_value="p1"):
            steps = {label: argv for label, _mod, argv in mgs.plan(args(ctest_outcome="skipped"))}
        self.assertIsNone(steps["affected-tests"])
        with mock.patch.object(mgs, "_first_parent", return_value=None):
            steps = {label: argv for label, _mod, argv in mgs.plan(args())}
        self.assertIsNone(steps["affected-tests"])


class IsolationTests(unittest.TestCase):
    def test_a_raising_instrument_is_named_and_the_runner_exits_0(self) -> None:
        boom = mock.MagicMock(); boom.main.side_effect = RuntimeError("no network")
        ok = mock.MagicMock(); ok.main.return_value = 0
        modules = {"binary_identity_shadow": boom, "affected_tests_shadow": ok, "flake_exoneration_shadow": ok,
                   "test_receipts_shadow": ok}
        with mock.patch.object(mgs, "_first_parent", return_value="p1"), \
                mock.patch.object(mgs, "load_module", side_effect=lambda name: modules[name]), \
                mock.patch("sys.stdout") as out:
            rc = mgs.main(["merge_group_shadows", "run", "--build-dir", "/b", "--source-root", "/s", "--repository", "O/R",
                           "--merge-sha", "m", "--token", "t", "--junit", "/j", "--selected-json", "/i",
                           "--ctest-outcome", "failure"])
        self.assertEqual(rc, 0)
        printed = "".join(c.args[0] for c in out.write.call_args_list if c.args)
        self.assertIn("binary-identity=error", printed)
        self.assertIn("affected-tests=rc=0", printed)
        self.assertIn("flake-exoneration=rc=0", printed)
        self.assertIn("test-receipts=rc=0", printed)

    def test_cli_exits_0_even_when_every_instrument_lacks_its_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = subprocess.run([sys.executable, str(HERE / "merge_group_shadows.py"), "run",
                                   "--build-dir", tmp, "--source-root", tmp, "--repository", "O/R",
                                   "--merge-sha", "0" * 40, "--token", "t", "--junit", f"{tmp}/none.xml",
                                   "--selected-json", f"{tmp}/none.json", "--ctest-outcome", "failure",
                                   "--work-dir", tmp], capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stderr[-800:])
        self.assertIn("merge-group shadows: binary-identity=", proc.stdout)
        self.assertIn("flake-exoneration=", proc.stdout)


if __name__ == "__main__":
    unittest.main()
