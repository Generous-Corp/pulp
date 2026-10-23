#!/usr/bin/env python3
"""Tests for tools/ci/ccache_remote_storage.py.

What must hold:
- with PULP_CCACHE_REMOTE_STORAGE unset or blank nothing is written, so a
  runner without shared storage behaves exactly as before;
- only code already on main writes: push, schedule and workflow_dispatch on
  refs/heads/main, and merge_group;
- every other event, and a trusted event on any other ref, gets the same URL
  with `|read-only`;
- operator attributes survive, but an operator `read-only` and anything that
  is not a ccache URL are refused with exit 2 and write nothing.

Run:
    python3 tools/ci/test_ccache_remote_storage.py
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import ccache_remote_storage as crs  # noqa: E402

SCRIPT = HERE / "ccache_remote_storage.py"
URL = "http://cache.invalid:8080/pulp-windows-x64"
MAIN = "refs/heads/main"


class SettingTests(unittest.TestCase):
    def test_trusted_events_on_main_write(self) -> None:
        for event in ("push", "schedule", "workflow_dispatch"):
            with self.subTest(event=event):
                self.assertEqual(crs.remote_storage_setting(URL, event, MAIN), URL)

    def test_merge_group_writes(self) -> None:
        ref = "refs/heads/gh-readonly-queue/main/pr-1-abc"
        self.assertEqual(crs.remote_storage_setting(URL, "merge_group", ref), URL)

    def test_pull_requests_read_only(self) -> None:
        for event in ("pull_request", "pull_request_target"):
            with self.subTest(event=event):
                self.assertEqual(crs.remote_storage_setting(URL, event, "refs/pull/7/merge"),
                                 URL + "|read-only")

    def test_trusted_event_on_other_branch_read_only(self) -> None:
        for event in ("push", "workflow_dispatch", "schedule"):
            with self.subTest(event=event):
                self.assertEqual(
                    crs.remote_storage_setting(URL, event, "refs/heads/feature/x"),
                    URL + "|read-only")

    def test_unknown_event_read_only(self) -> None:
        self.assertEqual(crs.remote_storage_setting(URL, "", ""), URL + "|read-only")

    def test_operator_attributes_kept(self) -> None:
        url = URL + "|connect-timeout=500"
        self.assertEqual(crs.remote_storage_setting(url, "pull_request", "refs/pull/1/merge"),
                         url + "|read-only")

    def test_operator_read_only_refused(self) -> None:
        with self.assertRaises(ValueError):
            crs.remote_storage_setting(URL + "|read-only", "push", MAIN)

    def test_non_url_refused(self) -> None:
        for bad in ("cache.invalid:8080", "http://a b/c", "http://x/y\nEVIL=1", "ftp://x/y"):
            with self.subTest(value=bad):
                with self.assertRaises(ValueError):
                    crs.remote_storage_setting(bad, "push", MAIN)


class CliTests(unittest.TestCase):
    def run_cli(self, env_value: str | None, event: str, ref: str) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            github_env = Path(tmp) / "env"
            github_env.write_text("")
            env = {k: v for k, v in os.environ.items() if k != "PULP_CCACHE_REMOTE_STORAGE"}
            env.update(GITHUB_EVENT_NAME=event, GITHUB_REF=ref)
            if env_value is not None:
                env["PULP_CCACHE_REMOTE_STORAGE"] = env_value
            result = subprocess.run([sys.executable, str(SCRIPT), "--github-env", str(github_env)],
                                    env=env, capture_output=True, text=True, check=False)
            return result.returncode, result.stdout, github_env.read_text()

    def test_unconfigured_writes_nothing(self) -> None:
        for value in (None, "", "   "):
            with self.subTest(value=value):
                rc, out, written = self.run_cli(value, "push", MAIN)
                self.assertEqual(rc, 0)
                self.assertEqual(written, "")
                self.assertIn("not configured", out)

    def test_trusted_writes_read_write(self) -> None:
        rc, out, written = self.run_cli(URL, "push", MAIN)
        self.assertEqual(rc, 0)
        self.assertEqual(written, f"CCACHE_REMOTE_STORAGE={URL}\n")
        self.assertIn("read-write", out)
        self.assertNotIn("cache.invalid", out)

    def test_pull_request_writes_read_only(self) -> None:
        rc, out, written = self.run_cli(URL, "pull_request", "refs/pull/9/merge")
        self.assertEqual(rc, 0)
        self.assertEqual(written, f"CCACHE_REMOTE_STORAGE={URL}|read-only\n")
        self.assertIn("read-only", out)

    def test_malformed_exits_2_and_writes_nothing(self) -> None:
        rc, _, written = self.run_cli("not a url", "push", MAIN)
        self.assertEqual(rc, 2)
        self.assertEqual(written, "")


if __name__ == "__main__":
    unittest.main()
