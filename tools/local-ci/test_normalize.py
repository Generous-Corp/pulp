#!/usr/bin/env python3
"""Tests for local CI normalization helpers."""

import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("normalize.py")


def load_module():
    spec = importlib.util.spec_from_file_location("pulp_local_ci_normalize", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class NormalizeTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_priority_and_mode_normalization(self):
        self.assertEqual(self.mod.normalize_priority(None), "normal")
        self.assertEqual(self.mod.normalize_priority(" HIGH "), "high")
        self.assertEqual(self.mod.priority_value("low"), 10)
        self.assertEqual(self.mod.priority_value("normal"), 50)
        self.assertEqual(self.mod.priority_value("high"), 100)

        with self.assertRaisesRegex(ValueError, "Invalid priority"):
            self.mod.normalize_priority("urgent")

        self.assertEqual(self.mod.normalize_validation_mode(None), "full")
        self.assertEqual(self.mod.normalize_validation_mode(" SMOKE "), "smoke")
        with self.assertRaisesRegex(ValueError, "Invalid validation mode"):
            self.mod.normalize_validation_mode("quick")

        self.assertEqual(self.mod.normalize_desktop_source_mode(None), "live")
        self.assertEqual(self.mod.normalize_desktop_source_mode(" exact_sha "), "exact-sha")
        with self.assertRaisesRegex(ValueError, "Invalid desktop source mode"):
            self.mod.normalize_desktop_source_mode("branch")

    def test_default_desktop_artifact_root_uses_platform_conventions(self):
        home = Path("/Users/pulp")

        with mock.patch.dict(
            self.mod.os.environ,
            {"PULP_DESKTOP_ARTIFACT_ROOT": "/override/runs"},
            clear=True,
        ):
            self.assertEqual(
                self.mod.default_desktop_artifact_root(),
                Path("/override/runs"),
            )

        with mock.patch.dict(self.mod.os.environ, {}, clear=True), mock.patch.object(
            self.mod.Path,
            "home",
            return_value=home,
        ), mock.patch.object(self.mod.sys, "platform", "darwin"):
            self.assertEqual(
                self.mod.default_desktop_artifact_root(),
                home / "Library" / "Application Support" / "Pulp" / "desktop-automation" / "runs",
            )

        with mock.patch.dict(
            self.mod.os.environ,
            {"LOCALAPPDATA": "C:/Users/pulp/AppData/Local"},
            clear=True,
        ), mock.patch.object(self.mod.sys, "platform", "win32"):
            self.assertEqual(
                self.mod.default_desktop_artifact_root(),
                Path("C:/Users/pulp/AppData/Local") / "Pulp" / "desktop-automation" / "runs",
            )

        with mock.patch.dict(
            self.mod.os.environ,
            {"XDG_STATE_HOME": "/windows-state"},
            clear=True,
        ), mock.patch.object(self.mod.sys, "platform", "win32"):
            self.assertEqual(
                self.mod.default_desktop_artifact_root(),
                Path("/windows-state") / "pulp" / "desktop-automation" / "runs",
            )

        with mock.patch.dict(
            self.mod.os.environ,
            {"XDG_STATE_HOME": "/state-home"},
            clear=True,
        ), mock.patch.object(self.mod.sys, "platform", "linux"):
            self.assertEqual(
                self.mod.default_desktop_artifact_root(),
                Path("/state-home") / "pulp" / "desktop-automation" / "runs",
            )

        with mock.patch.dict(self.mod.os.environ, {}, clear=True), mock.patch.object(
            self.mod.Path,
            "home",
            return_value=Path("/home/pulp"),
        ), mock.patch.object(self.mod.sys, "platform", "linux"):
            self.assertEqual(
                self.mod.default_desktop_artifact_root(),
                Path("/home/pulp/.local/state/pulp/desktop-automation/runs"),
            )

    def test_publish_modes_and_boolean_parser(self):
        self.assertEqual(self.mod.normalize_publish_mode(None), "none")
        self.assertEqual(self.mod.normalize_publish_mode(" PR-Comment "), "pr-comment")
        with self.assertRaisesRegex(ValueError, "Invalid desktop publish mode"):
            self.mod.normalize_publish_mode("slack")

        truthy = [True, 1, 2.0, "1", "true", "YES", " on "]
        falsy = [False, 0, 0.0, "", None, "0", "false", "NO", " off "]
        for value in truthy:
            self.assertTrue(self.mod.parse_config_bool(value), value)
        for value in falsy:
            self.assertFalse(self.mod.parse_config_bool(value), value)
        with self.assertRaisesRegex(ValueError, "Invalid boolean value"):
            self.mod.parse_config_bool("maybe")

    def test_optional_config_and_adapter_defaults(self):
        optional = self.mod.normalize_desktop_optional_config(
            {
                "webview_driver": "yes",
                "webdriver_url": "  http://localhost:4444  ",
                "debug_attach": 1,
                "debugger_command": "  lldb  ",
                "video_capture": "on",
                "frame_stats": False,
            }
        )
        self.assertEqual(
            optional,
            {
                "webview_driver": True,
                "webdriver_url": "http://localhost:4444",
                "debug_attach": True,
                "debugger_command": "lldb",
                "video_capture": True,
                "frame_stats": False,
            },
        )

        cases = [
            ("mac", {"type": "local"}, "macos-local", "launchagent", "v2"),
            ("ubuntu", {"type": "ssh"}, "linux-xvfb", "xvfb-run", "v2"),
            ("windows", {"type": "ssh"}, "windows-session-agent", "scheduled-task", "v2"),
            ("custom", {"type": "local"}, "local-window", "local-process", "v1"),
            ("remote", {"type": "ssh"}, "remote-session-agent", "ssh-bootstrap", "v1"),
            ("mystery", {}, "unknown", "manual", "v1"),
        ]
        for name, target_cfg, adapter, bootstrap, tier in cases:
            self.assertEqual(self.mod.infer_desktop_adapter(name, target_cfg), adapter)
            self.assertEqual(self.mod.default_desktop_bootstrap(adapter), bootstrap)
            self.assertEqual(self.mod.default_desktop_capability_tier(adapter), tier)

    def test_normalize_desktop_config_applies_defaults_and_overrides(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = {
                "desktop_automation": {
                    "artifact_root": str(root / "artifacts"),
                    "publish_mode": "branch",
                    "publish_branch": "desktop-evidence",
                    "retention_days": "7",
                    "targets": {
                        "mac": {
                            "enabled": False,
                            "adapter": "custom-adapter",
                            "bootstrap": "manual-script",
                            "capability_tier": "v9",
                            "host": "override-host",
                            "repo_path": "/override/repo",
                            "task_name": "DesktopProbe",
                            "remote_root": "/remote/root",
                            "optional": {
                                "webview_driver": "true",
                                "video_capture": "yes",
                            },
                        },
                        "ubuntu": {
                            "optional": {
                                "frame_stats": 1,
                            },
                        },
                    },
                },
                "targets": {
                    "mac": {
                        "type": "local",
                        "enabled": True,
                        "host": "mac-host",
                        "repo_path": "/repo",
                    },
                    "ubuntu": {
                        "type": "ssh",
                        "enabled": True,
                        "host": "ubuntu-host",
                        "repo_path": "/srv/pulp",
                    },
                },
            }

            normalized = self.mod.normalize_desktop_config(config)

        desktop = normalized["desktop_automation"]
        self.assertEqual(desktop["artifact_root"], str(root / "artifacts"))
        self.assertEqual(desktop["publish_mode"], "branch")
        self.assertEqual(desktop["publish_branch"], "desktop-evidence")
        self.assertEqual(desktop["retention_days"], 7)

        mac = desktop["targets"]["mac"]
        self.assertFalse(mac["enabled"])
        self.assertEqual(mac["adapter"], "custom-adapter")
        self.assertEqual(mac["bootstrap"], "manual-script")
        self.assertEqual(mac["capability_tier"], "v9")
        self.assertEqual(mac["host"], "override-host")
        self.assertEqual(mac["repo_path"], "/override/repo")
        self.assertEqual(mac["target_type"], "local")
        self.assertEqual(mac["task_name"], "DesktopProbe")
        self.assertEqual(mac["remote_root"], "/remote/root")
        self.assertTrue(mac["optional"]["webview_driver"])
        self.assertTrue(mac["optional"]["video_capture"])

        ubuntu = desktop["targets"]["ubuntu"]
        self.assertTrue(ubuntu["enabled"])
        self.assertEqual(ubuntu["adapter"], "linux-xvfb")
        self.assertEqual(ubuntu["bootstrap"], "xvfb-run")
        self.assertEqual(ubuntu["capability_tier"], "v2")
        self.assertEqual(ubuntu["host"], "ubuntu-host")
        self.assertEqual(ubuntu["repo_path"], "/srv/pulp")
        self.assertEqual(ubuntu["target_type"], "ssh")
        self.assertTrue(ubuntu["optional"]["frame_stats"])

        with mock.patch.object(
            self.mod,
            "default_desktop_artifact_root",
            return_value=Path("/default/artifacts"),
        ):
            defaulted = self.mod.normalize_desktop_config({"targets": {}})
        self.assertEqual(
            defaulted["desktop_automation"]["artifact_root"],
            "/default/artifacts",
        )


if __name__ == "__main__":
    unittest.main()
