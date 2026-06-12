#!/usr/bin/env python3
"""Tests for local CI notification helpers."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import unittest


MODULE_PATH = Path(__file__).with_name("notifications.py")


def load_module():
    spec = importlib.util.spec_from_file_location("notifications_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class NotificationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_notify_rings_terminal_and_posts_macos_notification(self) -> None:
        printed = []
        runs = []

        def print_fn(*args, **kwargs):
            printed.append((args, kwargs))

        def run_fn(cmd, **kwargs):
            runs.append((cmd, kwargs))
            return subprocess.CompletedProcess(cmd, 0)

        self.mod.notify("CI passed", print_fn=print_fn, run_fn=run_fn)

        self.assertEqual(printed, [(("\a",), {"end": "", "flush": True})])
        self.assertEqual(runs[0][0], ["osascript", "-e", 'display notification "CI passed" with title "Pulp CI"'])
        self.assertTrue(runs[0][1]["capture_output"])
        self.assertEqual(runs[0][1]["timeout"], 5)

    def test_notify_ignores_notification_failures(self) -> None:
        printed = []

        def run_fn(*_args, **_kwargs):
            raise RuntimeError("osascript unavailable")

        self.mod.notify("CI failed", print_fn=lambda *args, **kwargs: printed.append((args, kwargs)), run_fn=run_fn)

        self.assertEqual(printed, [(("\a",), {"end": "", "flush": True})])


if __name__ == "__main__":
    unittest.main()
