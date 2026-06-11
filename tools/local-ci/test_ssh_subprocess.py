#!/usr/bin/env python3
"""Tests for SSH subprocess retry helpers."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import unittest


MODULE_PATH = Path(__file__).with_name("ssh_subprocess.py")


def load_module():
    spec = importlib.util.spec_from_file_location("ssh_subprocess_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class SshSubprocessTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_transient_failure_detail_matches_known_ssh_resets(self) -> None:
        self.assertTrue(self.mod.is_transient_ssh_failure_detail("kex_exchange_identification: read: Connection reset by peer"))
        self.assertTrue(self.mod.is_transient_ssh_failure_detail("Connection timed out during banner exchange"))
        self.assertFalse(self.mod.is_transient_ssh_failure_detail("permission denied"))

    def test_run_ssh_subprocess_retries_transient_failures(self) -> None:
        calls = []
        sleeps = []

        def run_fn(*args, **kwargs):
            calls.append((args, kwargs))
            if len(calls) == 1:
                return subprocess.CompletedProcess(args[0], 255, stdout="", stderr="Connection reset by peer")
            return subprocess.CompletedProcess(args[0], 0, stdout="ok", stderr="")

        result = self.mod.run_ssh_subprocess(
            ["ssh", "host"],
            input="payload",
            timeout=5,
            retry_delay_secs=0.25,
            run_fn=run_fn,
            sleep_fn=sleeps.append,
        )

        self.assertEqual(result.stdout, "ok")
        self.assertEqual(len(calls), 2)
        self.assertEqual(sleeps, [0.25])
        self.assertEqual(calls[0][1]["input"], "payload")
        self.assertEqual(calls[0][1]["timeout"], 5)

    def test_run_ssh_subprocess_does_not_retry_permanent_failures(self) -> None:
        calls = []
        sleeps = []
        permanent = subprocess.CompletedProcess(["ssh", "host"], 255, stdout="", stderr="permission denied")

        def run_fn(*args, **kwargs):
            calls.append((args, kwargs))
            return permanent

        self.assertIs(self.mod.run_ssh_subprocess(["ssh", "host"], run_fn=run_fn, sleep_fn=sleeps.append), permanent)
        self.assertEqual(len(calls), 1)
        self.assertEqual(sleeps, [])


if __name__ == "__main__":
    unittest.main()
