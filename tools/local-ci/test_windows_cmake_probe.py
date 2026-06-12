#!/usr/bin/env python3
"""Tests for Windows CMake generator probe helpers."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_cmake_probe.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


def completed(*, returncode: int = 0, stdout: str = "", stderr: str = ""):
    return SimpleNamespace(returncode=returncode, stdout=stdout, stderr=stderr)


class WindowsCmakeProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_skips_remote_when_platform_and_instance_supplied(self) -> None:
        def fail_if_called(*args, **kwargs):
            raise AssertionError("probe should not run when both values are supplied")

        self.assertEqual(
            self.mod.probe_windows_ssh_cmake_settings(
                "win",
                "Visual Studio 17 2022",
                "x64",
                "C:/VS",
                windows_ssh_powershell_command_fn=lambda host: ["ssh", host],
                run_fn=fail_if_called,
            ),
            ("x64", "C:/VS"),
        )

    def test_probe_parses_remote_json_and_falls_back_on_errors(self) -> None:
        captured: dict = {}

        def fake_run(args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return completed(stdout='noise\n{"platform":"ARM64","generator_instance":"C:/VS/Community"}\n')

        self.assertEqual(
            self.mod.probe_windows_ssh_cmake_settings(
                "win",
                "Visual Studio 17 2022",
                "",
                "",
                windows_ssh_powershell_command_fn=lambda host: ["ssh", host],
                run_fn=fake_run,
            ),
            ("ARM64", "C:/VS/Community"),
        )
        self.assertEqual(captured["args"], ["ssh", "win"])
        self.assertEqual(captured["kwargs"]["timeout"], 60)
        self.assertIn("Resolve-CMakePlatform", captured["kwargs"]["input"])

        def fake_raises(*args, **kwargs):
            raise OSError("ssh unavailable")

        self.assertEqual(
            self.mod.probe_windows_ssh_cmake_settings(
                "win",
                "Ninja",
                "x64",
                "",
                windows_ssh_powershell_command_fn=lambda host: ["ssh", host],
                run_fn=fake_raises,
            ),
            ("x64", ""),
        )

        self.assertEqual(
            self.mod.probe_windows_ssh_cmake_settings(
                "win",
                "Ninja",
                "",
                "C:/VS",
                windows_ssh_powershell_command_fn=lambda host: ["ssh", host],
                run_fn=lambda *_args, **_kwargs: completed(returncode=1),
            ),
            ("", "C:/VS"),
        )


if __name__ == "__main__":
    unittest.main()
