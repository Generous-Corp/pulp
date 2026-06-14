#!/usr/bin/env python3
"""No-network tests for Linux remote desktop doctor checks."""

from __future__ import annotations

import pathlib
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("desktop_doctor_remote_linux.py")


def load_module():
    return load_module_from_path(MODULE_PATH, add_module_dir=True)


def check(name: str, ok: bool, detail: str, *, required: bool = True) -> dict:
    return {"name": name, "ok": ok, "detail": detail, "required": required}


class DesktopDoctorRemoteLinuxTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_linux_remote_doctor_checks_builds_backend_and_tooling_rows(self) -> None:
        checks = self.mod.linux_remote_doctor_checks(
            host="ubuntu.example",
            probe_linux_launch_backend_fn=lambda _host: {"mode": "display", "display": ":99"},
            probe_linux_remote_tooling_fn=lambda _host: {
                "git_found": True,
                "git_path": "/usr/bin/git",
                "git_lfs_found": False,
                "xvfb_run_found": True,
                "xvfb_run_path": "/usr/bin/xvfb-run",
                "xauth_found": True,
                "xdotool_found": True,
                "xwininfo_found": True,
                "import_found": True,
                "wmctrl_found": False,
            },
            desktop_check_fn=check,
        )
        by_name = {item["name"]: item for item in checks}

        self.assertTrue(by_name["launch_backend"]["ok"])
        self.assertEqual(by_name["launch_backend"]["detail"], "existing display :99")
        self.assertTrue(by_name["git"]["ok"])
        self.assertFalse(by_name["git-lfs"]["ok"])
        self.assertIn("sudo apt-get install -y git-lfs", by_name["git-lfs"]["detail"])
        self.assertFalse(by_name["wmctrl"]["required"])

    def test_linux_remote_doctor_checks_reports_probe_errors(self) -> None:
        checks = self.mod.linux_remote_doctor_checks(
            host="ubuntu.example",
            probe_linux_launch_backend_fn=lambda _host: (_ for _ in ()).throw(RuntimeError("backend failed")),
            probe_linux_remote_tooling_fn=lambda _host: (_ for _ in ()).throw(RuntimeError("tooling failed")),
            desktop_check_fn=check,
        )
        by_name = {item["name"]: item for item in checks}
        self.assertEqual(by_name["launch_backend"]["detail"], "backend failed")
        self.assertEqual(by_name["remote_tooling"]["detail"], "tooling failed")


if __name__ == "__main__":
    unittest.main()
