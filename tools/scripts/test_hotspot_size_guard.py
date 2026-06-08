#!/usr/bin/env python3
"""Tests for hotspot_size_guard.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import runpy
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


THIS_DIR = Path(__file__).parent
SCRIPT = THIS_DIR / "hotspot_size_guard.py"


def load_module():
    spec = importlib.util.spec_from_file_location("hotspot_size_guard", str(SCRIPT))
    module = importlib.util.module_from_spec(spec)
    sys.modules["hotspot_size_guard"] = module
    spec.loader.exec_module(module)
    return module


hsg = load_module()


def git(cwd: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(cwd), *args], check=True, capture_output=True, text=True)


class HotspotSizeGuardUnitTests(unittest.TestCase):
    def test_load_config_rejects_duplicate_hotspots(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config = Path(td) / "config.json"
            config.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "hotspots": [
                            {"path": "core/a.cpp", "max_loc": 1},
                            {"path": "core/a.cpp", "max_loc": 2},
                        ],
                    }
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "duplicate hotspot path"):
                hsg.load_config(config)

    def test_load_config_rejects_non_object_root(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            config = Path(td) / "config.json"
            config.write_text("[]\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "config root must be an object"):
                hsg.load_config(config)

    def test_hint_mode_stays_green_outside_git_worktree(self) -> None:
        with mock.patch.object(hsg, "repo_root", return_value=None):
            self.assertEqual(hsg.main(["--mode", "hint"]), 0)
            self.assertEqual(hsg.main(["--mode", "report"]), 2)


class HotspotSizeGuardIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="pulp-hotspot-size-guard-"))
        self.cwd = os.getcwd()
        os.chdir(self.tmp)
        git(self.tmp, "init", "-q", "-b", "main")
        git(self.tmp, "config", "user.email", "test@example.com")
        git(self.tmp, "config", "user.name", "Test")
        git(self.tmp, "config", "commit.gpgsign", "false")
        self.config = self.tmp / "guard.json"

    def tearDown(self) -> None:
        os.chdir(self.cwd)
        import shutil

        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_config(self, max_loc: int = 3, new_file_max: int = 5) -> None:
        self.config.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "new_file_warning": {
                        "max_loc": new_file_max,
                        "paths": ["core/**", "tools/**"],
                    },
                    "hotspots": [
                        {
                            "path": "core/view/src/widget_bridge.cpp",
                            "max_loc": max_loc,
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )

    def run_guard(self, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--base",
                "origin/main",
                "--config",
                str(self.config),
                *extra,
            ],
            cwd=self.tmp,
            capture_output=True,
            text=True,
            check=False,
        )

    def commit_baseline(self) -> None:
        hotspot = self.tmp / "core" / "view" / "src" / "widget_bridge.cpp"
        hotspot.parent.mkdir(parents=True)
        hotspot.write_text("a\nb\nc\n", encoding="utf-8")
        self.write_config(max_loc=3)
        git(self.tmp, "add", ".")
        git(self.tmp, "commit", "-q", "-m", "baseline")
        git(self.tmp, "update-ref", "refs/remotes/origin/main", "HEAD")

    def test_current_baseline_passes(self) -> None:
        self.commit_baseline()

        result = self.run_guard()

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("hotspot_size_guard: ok", result.stdout)

    def test_one_line_hotspot_growth_fails(self) -> None:
        self.commit_baseline()
        hotspot = self.tmp / "core" / "view" / "src" / "widget_bridge.cpp"
        hotspot.write_text("a\nb\nc\nd\n", encoding="utf-8")

        result = self.run_guard()

        self.assertEqual(result.returncode, 1)
        self.assertIn("4 LOC exceeds frozen ceiling 3", result.stderr)

    def test_large_new_core_file_warns_without_failing(self) -> None:
        self.commit_baseline()
        added = self.tmp / "core" / "render" / "src" / "new_renderer.cpp"
        added.parent.mkdir(parents=True, exist_ok=True)
        added.write_text("1\n2\n3\n4\n5\n6\n", encoding="utf-8")
        git(self.tmp, "add", str(added.relative_to(self.tmp)))
        git(self.tmp, "commit", "-q", "-m", "add large file")

        result = self.run_guard()

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("large new-file warning", result.stderr)
        self.assertIn("new_renderer.cpp", result.stderr)

    def test_warnings_can_be_promoted_to_errors(self) -> None:
        self.commit_baseline()
        added = self.tmp / "tools" / "new_tool.py"
        added.parent.mkdir(parents=True, exist_ok=True)
        added.write_text("1\n2\n3\n4\n5\n6\n", encoding="utf-8")
        git(self.tmp, "add", str(added.relative_to(self.tmp)))
        git(self.tmp, "commit", "-q", "-m", "add large tool")

        result = self.run_guard("--warnings-as-errors")

        self.assertEqual(result.returncode, 1)
        self.assertIn("new_tool.py", result.stderr)

    def test_script_entrypoint_exits_with_main_status(self) -> None:
        proc = subprocess.CompletedProcess(["git"], 1, stdout="", stderr="fatal")
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [str(SCRIPT), "--mode", "report"]), \
             mock.patch.object(subprocess, "run", return_value=proc), \
             contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as cm:
                runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(cm.exception.code, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
