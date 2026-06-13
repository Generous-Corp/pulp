#!/usr/bin/env python3
"""Unit tests for run_reaper_hostbench_smoke.py."""

from __future__ import annotations

import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).resolve().parent / "run_reaper_hostbench_smoke.py"
spec = importlib.util.spec_from_file_location("run_reaper_hostbench_smoke", SCRIPT)
assert spec and spec.loader
runner = importlib.util.module_from_spec(spec)
sys.modules["run_reaper_hostbench_smoke"] = runner
spec.loader.exec_module(runner)


class ReaperHostBenchSmokeTests(unittest.TestCase):
    def test_generated_lua_uses_format_specific_fx_name(self) -> None:
        vst3 = runner.build_lua_script(runner.FORMAT_CONFIGS["vst3"])
        clap = runner.build_lua_script(runner.FORMAT_CONFIGS["clap"])

        self.assertIn("VST3: PulpHostBench (Pulp)", vst3)
        self.assertIn("CLAP: PulpHostBench (Pulp)", clap)
        self.assertIn("TrackFX_AddByName", vst3)
        self.assertIn("PULP_REAPER_HOSTBENCH_STATUS", clap)
        self.assertIn("Transport: Play", vst3)

    def test_newest_added_log_ignores_existing_logs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            old = root / "REAPER-VST3-old.log"
            old.write_text("old", encoding="utf-8")
            before = runner.snapshot_logs(root, "REAPER-VST3-*.log")

            time.sleep(0.01)
            new = root / "REAPER-VST3-new.log"
            new.write_text("new", encoding="utf-8")

            self.assertEqual(
                runner.newest_added_log(before, root, "REAPER-VST3-*.log"),
                new.resolve(),
            )

    def test_status_success_requires_fx_and_done(self) -> None:
        self.assertTrue(runner.status_has_success("start version=7.74\nfx=0 name=x\ndone ticks=24\n"))
        self.assertFalse(runner.status_has_success("start version=7.74\nfx=-1 name=\n"))
        self.assertFalse(runner.status_has_success("start version=7.74\nerror=no_track\n"))
        self.assertFalse(runner.status_has_success("start version=7.74\nfx=0 name=x\n"))

    def test_log_event_counts_and_required_events(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "REAPER-VST3.log"
            path.write_text(
                "2026-06-13T00:00:00Z\tsession_start\tn=1\n"
                "2026-06-13T00:00:00Z\tprocessor_construct\tn=2\n"
                "2026-06-13T00:00:00Z\tdefine_parameters\tn=3\n"
                "2026-06-13T00:00:00Z\tprepare\tn=4\n"
                "2026-06-13T00:00:00Z\tprepare\tn=5\n",
                encoding="utf-8",
            )
            counts = runner.log_event_counts(path)
            self.assertEqual(counts["prepare"], 2)
            self.assertEqual(runner.missing_required_events(counts), [])
            del counts["prepare"]
            self.assertEqual(runner.missing_required_events(counts), ["prepare"])

    def test_print_lua_mode_does_not_require_reaper_binary(self) -> None:
        with mock.patch.object(sys, "stdout", io.StringIO()) as stdout:
            self.assertEqual(runner.main([
                "--format", "vst3",
                "--reaper", "/definitely/missing/REAPER",
                "--print-lua",
            ]), 0)
            self.assertIn("TrackFX_AddByName", stdout.getvalue())

    def test_missing_reaper_binary_fails_before_creating_logs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, \
             mock.patch.object(sys, "stderr", io.StringIO()) as stderr:
            rc = runner.main([
                "--format", "vst3",
                "--reaper", str(pathlib.Path(tmp) / "missing-reaper"),
                "--log-dir", str(pathlib.Path(tmp) / "logs"),
            ])
            self.assertEqual(rc, 2)
            self.assertIn("missing REAPER binary", stderr.getvalue())
            self.assertFalse((pathlib.Path(tmp) / "logs").exists())

    def test_run_reaper_returns_timeout_result_after_killing_process(self) -> None:
        proc = mock.Mock()
        proc.args = ["REAPER"]
        proc.communicate.side_effect = [
            subprocess.TimeoutExpired(cmd=["REAPER"], timeout=0.1),
            ("late stdout", "late stderr"),
        ]
        proc.returncode = None

        with mock.patch.object(runner.subprocess, "Popen", return_value=proc):
            result = runner.run_reaper(
                reaper=pathlib.Path("/Applications/REAPER.app/Contents/MacOS/REAPER"),
                script=pathlib.Path("/tmp/script.lua"),
                status_file=pathlib.Path("/tmp/status.txt"),
                timeout_sec=0.1,
            )

        proc.kill.assert_called_once()
        self.assertEqual(result.returncode, 124)
        self.assertEqual(result.stdout, "late stdout")
        self.assertEqual(result.stderr, "late stderr")

    def test_main_reports_success_payload_and_copies_log(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            reaper = root / "REAPER"
            reaper.write_text("#!/bin/sh\n", encoding="utf-8")
            log_dir = root / "logs"
            copy_dir = root / "copied"

            def fake_run_reaper(*, status_file: pathlib.Path, **_: object) -> subprocess.CompletedProcess[str]:
                status_file.write_text("fx=0 name=VST3: PulpHostBench (Pulp)\ndone ticks=24\n", encoding="utf-8")
                log_dir.mkdir(parents=True, exist_ok=True)
                (log_dir / "REAPER-VST3-new.log").write_text(
                    "2026-06-13T00:00:00Z\tsession_start\tn=1\n"
                    "2026-06-13T00:00:00Z\tprocessor_construct\tn=1\n"
                    "2026-06-13T00:00:00Z\tdefine_parameters\tn=1\n"
                    "2026-06-13T00:00:00Z\tprepare\tn=1\n",
                    encoding="utf-8",
                )
                return subprocess.CompletedProcess(["REAPER"], 0, "stdout", "stderr")

            with mock.patch.object(runner, "run_reaper", side_effect=fake_run_reaper), \
                 mock.patch.object(sys, "stdout", new_callable=lambda: mock.Mock()) as fake_stdout:
                fake_stdout.write = mock.Mock()
                rc = runner.main([
                    "--format", "vst3",
                    "--reaper", str(reaper),
                    "--log-dir", str(log_dir),
                    "--copy-log-to", str(copy_dir),
                ])

            self.assertEqual(rc, 0)
            written = "".join(call.args[0] for call in fake_stdout.write.call_args_list)
            payload = json.loads(written)
            self.assertTrue(payload["ok"])
            self.assertEqual(payload["event_counts"]["prepare"], 1)
            self.assertEqual(payload["missing_required_events"], [])
            self.assertEqual(pathlib.Path(payload["log"]).parent, copy_dir)
            self.assertTrue((copy_dir / "REAPER-VST3-new.log").is_file())

    def test_main_reports_missing_required_events_as_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            reaper = root / "REAPER"
            reaper.write_text("#!/bin/sh\n", encoding="utf-8")
            log_dir = root / "logs"

            def fake_run_reaper(*, status_file: pathlib.Path, **_: object) -> subprocess.CompletedProcess[str]:
                status_file.write_text("fx=0 name=CLAP: PulpHostBench (Pulp)\ndone ticks=24\n", encoding="utf-8")
                log_dir.mkdir(parents=True, exist_ok=True)
                (log_dir / "REAPER-CLAP-new.log").write_text(
                    "2026-06-13T00:00:00Z\tsession_start\tn=1\n",
                    encoding="utf-8",
                )
                return subprocess.CompletedProcess(["REAPER"], 0, "", "")

            with mock.patch.object(runner, "run_reaper", side_effect=fake_run_reaper), \
                 mock.patch.object(sys, "stdout", new_callable=lambda: mock.Mock()) as fake_stdout:
                fake_stdout.write = mock.Mock()
                rc = runner.main([
                    "--format", "clap",
                    "--reaper", str(reaper),
                    "--log-dir", str(log_dir),
                ])

            self.assertEqual(rc, 1)
            written = "".join(call.args[0] for call in fake_stdout.write.call_args_list)
            payload = json.loads(written)
            self.assertFalse(payload["ok"])
            self.assertEqual(
                payload["missing_required_events"],
                ["processor_construct", "define_parameters", "prepare"],
            )


if __name__ == "__main__":
    unittest.main()
