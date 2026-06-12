#!/usr/bin/env python3
"""Tests for SSH subprocess facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import subprocess
import types
import unittest


MODULE_PATH = Path(__file__).with_name("ssh_subprocess_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class SshSubprocessBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_helpers_bind_facade_dependencies(self) -> None:
        captured = {}

        def is_transient(detail):
            captured["detail"] = detail
            return True

        def run_ssh_subprocess(args, **kwargs):
            captured["run"] = (args, kwargs)
            return subprocess.CompletedProcess(args, 0, stdout="ok", stderr="")

        fake_subprocess = types.SimpleNamespace(run=object())
        fake_time = types.SimpleNamespace(sleep=object())
        bindings = {
            "_ssh_subprocess": types.SimpleNamespace(
                is_transient_ssh_failure_detail=is_transient,
                run_ssh_subprocess=run_ssh_subprocess,
            ),
            "subprocess": fake_subprocess,
            "time": fake_time,
        }

        self.assertTrue(self.mod.is_transient_ssh_failure_detail(bindings, "reset"))
        result = self.mod.run_ssh_subprocess(
            bindings,
            ["ssh", "host"],
            input="payload",
            timeout=5,
            retries=4,
            retry_delay_secs=0.5,
        )

        self.assertEqual(result.stdout, "ok")
        self.assertEqual(captured["detail"], "reset")
        self.assertEqual(captured["run"][0], ["ssh", "host"])
        self.assertEqual(captured["run"][1]["input"], "payload")
        self.assertEqual(captured["run"][1]["timeout"], 5)
        self.assertEqual(captured["run"][1]["retries"], 4)
        self.assertEqual(captured["run"][1]["retry_delay_secs"], 0.5)
        self.assertIs(captured["run"][1]["run_fn"], fake_subprocess.run)
        self.assertIs(captured["run"][1]["sleep_fn"], fake_time.sleep)

    def test_install_ssh_subprocess_helpers_wires_named_exports(self) -> None:
        captured = {}

        def is_transient(detail):
            captured["detail"] = detail
            return True

        def run_ssh_subprocess(args, **kwargs):
            captured["run"] = (args, kwargs)
            return subprocess.CompletedProcess(args, 0, stdout="ok", stderr="")

        bindings = {
            "_ssh_subprocess": types.SimpleNamespace(
                is_transient_ssh_failure_detail=is_transient,
                run_ssh_subprocess=run_ssh_subprocess,
            ),
            "subprocess": types.SimpleNamespace(run=object()),
            "time": types.SimpleNamespace(sleep=object()),
        }
        self.mod.install_ssh_subprocess_helpers(bindings, ("is_transient_ssh_failure_detail", "run_ssh_subprocess"))

        self.assertTrue(bindings["is_transient_ssh_failure_detail"]("reset"))
        self.assertEqual(bindings["run_ssh_subprocess"](["ssh", "host"]).stdout, "ok")
        self.assertEqual(bindings["is_transient_ssh_failure_detail"].__name__, "is_transient_ssh_failure_detail")
        self.assertEqual(captured["detail"], "reset")
        self.assertEqual(captured["run"][0], ["ssh", "host"])


if __name__ == "__main__":
    unittest.main()
