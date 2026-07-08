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

    def test_repo_hotspot_config_paths_exist(self) -> None:
        root = THIS_DIR.parents[1]
        config = hsg.load_config(root / "tools" / "scripts" / "hotspot_size_guard.json")

        missing = [hotspot.path for hotspot in config.hotspots
                   if not (root / hotspot.path).is_file()]

        self.assertEqual(missing, [])


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

    def write_config(
        self,
        max_loc: int = 3,
        new_file_max: int = 5,
        include_hotspot: bool = True,
    ) -> None:
        hotspots = []
        if include_hotspot:
            hotspots.append(
                {
                    "path": "core/view/src/widget_bridge.cpp",
                    "max_loc": max_loc,
                }
            )
        else:
            hotspots.append(
                {
                    "path": "tools/scripts/other.py",
                    "max_loc": max_loc,
                }
            )
        self.config.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "new_file_warning": {
                        "max_loc": new_file_max,
                        "paths": ["core/**", "tools/**"],
                    },
                    "hotspots": hotspots,
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

    def commit_baseline(self, *, max_loc: int = 3, contents: str = "a\nb\nc\n") -> None:
        hotspot = self.tmp / "core" / "view" / "src" / "widget_bridge.cpp"
        hotspot.parent.mkdir(parents=True)
        hotspot.write_text(contents, encoding="utf-8")
        self.write_config(max_loc=max_loc)
        git(self.tmp, "add", ".")
        git(self.tmp, "commit", "-q", "-m", "baseline")
        git(self.tmp, "update-ref", "refs/remotes/origin/main", "HEAD")

    def commit_all(self, message: str) -> None:
        git(self.tmp, "add", ".")
        git(self.tmp, "commit", "-q", "-m", message)

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
        # Net-delta message: THIS PR grew the hotspot past its reference.
        self.assertIn("grows a frozen hotspot 3 -> 4 LOC", result.stderr)
        self.assertIn("Hotspot-Grow", result.stderr)

    def test_net_neutral_passes_even_when_file_is_over_reference(self) -> None:
        # The race-killer: main grew the hotspot past its (now stale) reference
        # via Hotspot-Grow trailers, not a max_loc bump. A PR that does NOT grow
        # the file must pass, even though its absolute size exceeds max_loc.
        self.commit_baseline(max_loc=3, contents="a\nb\nc\nd\ne\n")  # 5 LOC, ref 3

        result = self.run_guard()

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("did not grow it", result.stderr)

    def test_hotspot_grow_trailer_authorizes_growth(self) -> None:
        self.commit_baseline()  # 3 LOC, ref 3
        hotspot = self.tmp / "core" / "view" / "src" / "widget_bridge.cpp"
        hotspot.write_text("a\nb\nc\nd\n", encoding="utf-8")  # grow to 4
        git(self.tmp, "add", ".")
        git(self.tmp, "commit", "-q", "-m",
            "grow hotspot\n\nHotspot-Grow: core/view/src/widget_bridge.cpp reason=\"needed\"")

        result = self.run_guard()

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("authorized by Hotspot-Grow", result.stderr)

    def test_hotspot_shrink_reports_without_failing_by_default(self) -> None:
        self.commit_baseline()
        hotspot = self.tmp / "core" / "view" / "src" / "widget_bridge.cpp"
        hotspot.write_text("a\nb\n", encoding="utf-8")
        self.commit_all("shrink hotspot")

        result = self.run_guard()

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("tracked hotspot shrink", result.stderr)
        self.assertIn("3 -> 2 LOC", result.stderr)
        self.assertIn("ceiling unchanged", result.stderr)

    def test_require_ceiling_reduction_fails_when_shrink_keeps_old_ceiling(self) -> None:
        self.commit_baseline()
        hotspot = self.tmp / "core" / "view" / "src" / "widget_bridge.cpp"
        hotspot.write_text("a\nb\n", encoding="utf-8")
        self.commit_all("shrink hotspot")

        result = self.run_guard("--require-ceiling-reduction")

        self.assertEqual(result.returncode, 1)
        self.assertIn("missing hotspot ceiling reduction", result.stderr)
        self.assertIn("shrank 3 -> 2 LOC but max_loc is 3", result.stderr)
        self.assertIn("set max_loc to 2", result.stderr)

    def test_require_ceiling_reduction_passes_when_shrink_lowers_ceiling(self) -> None:
        self.commit_baseline()
        hotspot = self.tmp / "core" / "view" / "src" / "widget_bridge.cpp"
        hotspot.write_text("a\nb\n", encoding="utf-8")
        self.write_config(max_loc=2)
        self.commit_all("shrink hotspot and lower ceiling")

        result = self.run_guard("--require-ceiling-reduction")

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("max_loc 3 -> 2", result.stderr)
        self.assertIn("hotspot_size_guard: ok", result.stdout)

    def test_require_ceiling_reduction_fails_when_ceiling_keeps_headroom(self) -> None:
        self.commit_baseline(max_loc=5, contents="a\nb\nc\nd\ne\n")
        hotspot = self.tmp / "core" / "view" / "src" / "widget_bridge.cpp"
        hotspot.write_text("a\nb\nc\n", encoding="utf-8")
        self.write_config(max_loc=4)
        self.commit_all("shrink hotspot and leave ceiling slack")

        result = self.run_guard("--require-ceiling-reduction")

        self.assertEqual(result.returncode, 1)
        self.assertIn("shrank 5 -> 3 LOC but max_loc is 4", result.stderr)
        self.assertIn("set max_loc to 3", result.stderr)

    def test_require_ceiling_reduction_fails_when_shrink_removes_hotspot_entry(self) -> None:
        self.commit_baseline()
        hotspot = self.tmp / "core" / "view" / "src" / "widget_bridge.cpp"
        hotspot.write_text("a\nb\n", encoding="utf-8")
        self.write_config(include_hotspot=False)
        self.commit_all("shrink hotspot and remove ceiling")

        result = self.run_guard("--require-ceiling-reduction")

        self.assertEqual(result.returncode, 1)
        self.assertIn("tracked hotspot removal", result.stderr)
        self.assertIn("removed from config but still exists at 2 LOC", result.stderr)
        self.assertIn("restore its entry", result.stderr)
        self.assertIn("max_loc 2", result.stderr)

    def test_require_ceiling_reduction_fails_when_untouched_hotspot_entry_is_removed(self) -> None:
        self.commit_baseline()
        self.write_config(include_hotspot=False)
        self.commit_all("remove hotspot ceiling")

        result = self.run_guard("--require-ceiling-reduction")

        self.assertEqual(result.returncode, 1)
        self.assertIn("tracked hotspot removal", result.stderr)
        self.assertIn("removed from config but still exists at 3 LOC", result.stderr)
        self.assertIn("restore its entry", result.stderr)
        self.assertIn("max_loc 3", result.stderr)

    def test_require_ceiling_reduction_ignores_hotspot_drift_from_moving_base(self) -> None:
        self.commit_baseline()
        branch_tip = subprocess.run(
            ["git", "-C", str(self.tmp), "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()

        hotspot = self.tmp / "core" / "view" / "src" / "widget_bridge.cpp"
        hotspot.write_text("a\nb\nc\nd\n", encoding="utf-8")
        self.write_config(max_loc=4)
        self.commit_all("main grows hotspot")
        git(self.tmp, "update-ref", "refs/remotes/origin/main", "HEAD")
        git(self.tmp, "reset", "--hard", branch_tip)

        result = self.run_guard("--require-ceiling-reduction")

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertNotIn("missing hotspot ceiling reduction", result.stderr)

    def test_require_ceiling_reduction_skips_malformed_base_config(self) -> None:
        hotspot = self.tmp / "core" / "view" / "src" / "widget_bridge.cpp"
        hotspot.parent.mkdir(parents=True)
        hotspot.write_text("a\nb\nc\n", encoding="utf-8")
        self.config.write_text("{", encoding="utf-8")
        self.commit_all("malformed baseline")
        git(self.tmp, "update-ref", "refs/remotes/origin/main", "HEAD")

        self.write_config()
        self.commit_all("fix hotspot config")

        result = self.run_guard("--require-ceiling-reduction")

        self.assertEqual(result.returncode, 0, msg=result.stderr)
        self.assertIn("shrink check skipped", result.stderr)

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
