#!/usr/bin/env python3
"""Tests for Linux target facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("linux_target_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, linux_target):
        return {
            "_linux_target": linux_target,
            "ssh_command_result": object(),
            "LINUX_REQUIRED_REMOTE_TOOLS": {"git": {"required": True}},
            "parse_coordinate_pair": object(),
        }

    def test_probe_and_tooling_bind_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        linux_target = types.SimpleNamespace(
            LINUX_REQUIRED_REMOTE_TOOLS={"git": {"required": True}},
            LINUX_OPTIONAL_REMOTE_TOOLS={"xvfb": {"required": False}},
            probe_linux_launch_backend=capture("launch", {"mode": "xvfb"}),
            probe_linux_remote_tooling=capture("tooling", {"git_found": True}),
            linux_tooling_detail=capture("detail", "git version"),
            linux_remote_tooling_ready=capture("ready", True),
        )
        bindings = self._bindings(linux_target)
        probe = {"git_found": True}

        self.assertIs(self.mod.linux_required_remote_tools(bindings), linux_target.LINUX_REQUIRED_REMOTE_TOOLS)
        self.assertIs(self.mod.linux_optional_remote_tools(bindings), linux_target.LINUX_OPTIONAL_REMOTE_TOOLS)
        self.assertEqual(self.mod.probe_linux_launch_backend(bindings, "ubuntu"), {"mode": "xvfb"})
        self.assertEqual(captured["launch"][0], ("ubuntu",))
        self.assertIs(captured["launch"][1]["ssh_command_result_fn"], bindings["ssh_command_result"])
        self.assertEqual(self.mod.probe_linux_remote_tooling(bindings, "ubuntu"), {"git_found": True})
        self.assertIs(captured["tooling"][1]["ssh_command_result_fn"], bindings["ssh_command_result"])
        self.assertEqual(self.mod.linux_tooling_detail(bindings, probe, "git", missing_hint="install git"), "git version")
        self.assertEqual(captured["detail"][0], (probe, "git"))
        self.assertEqual(captured["detail"][1], {"missing_hint": "install git"})
        self.assertTrue(self.mod.linux_remote_tooling_ready(bindings, probe))
        self.assertEqual(captured["ready"][0], (probe,))
        self.assertIs(captured["ready"][1]["required_tools"], bindings["LINUX_REQUIRED_REMOTE_TOOLS"])

    def test_path_and_command_builders_delegate_with_parse_coordinate_seam(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        linux_target = types.SimpleNamespace(
            remote_linux_bundle_relpath=capture("relpath", ".local/state/pulp/desktop-automation/remote/ubuntu/smoke/run"),
            build_linux_xvfb_remote_command=capture("xvfb", "xvfb-cmd"),
            build_linux_window_driver_remote_command=capture("window", "window-cmd"),
        )
        bindings = self._bindings(linux_target)
        bundle_dir = Path("/tmp/run")

        self.assertEqual(
            self.mod.remote_linux_bundle_relpath(bindings, "ubuntu", "smoke", bundle_dir),
            ".local/state/pulp/desktop-automation/remote/ubuntu/smoke/run",
        )
        self.assertEqual(captured["relpath"][0], ("ubuntu", "smoke", bundle_dir))

        self.assertEqual(
            self.mod.build_linux_xvfb_remote_command(
                bindings,
                "/repo",
                ".local/run",
                "./app",
                launch_backend={"mode": "xvfb"},
                launch_cwd="/repo/build",
                capture_ui_snapshot=True,
                click_point="1,2",
                click_view_id="gain",
                click_view_type="Slider",
                click_view_text="Gain",
                click_view_label="Gain slider",
                capture_before=True,
                settle_secs=0.25,
            ),
            "xvfb-cmd",
        )
        xvfb_kwargs = captured["xvfb"][1]
        self.assertEqual(captured["xvfb"][0], ("/repo", ".local/run", "./app"))
        self.assertEqual(xvfb_kwargs["launch_backend"], {"mode": "xvfb"})
        self.assertTrue(xvfb_kwargs["capture_ui_snapshot"])
        self.assertEqual(xvfb_kwargs["click_view_label"], "Gain slider")

        self.assertEqual(
            self.mod.build_linux_window_driver_remote_command(
                bindings,
                "/repo",
                ".local/run",
                "./app",
                launch_backend={"mode": "display", "display": ":99"},
                launch_cwd="/repo/build",
                click_point="1,2",
                capture_before=True,
                settle_secs=0.25,
            ),
            "window-cmd",
        )
        window_kwargs = captured["window"][1]
        self.assertEqual(captured["window"][0], ("/repo", ".local/run", "./app"))
        self.assertEqual(window_kwargs["launch_backend"], {"mode": "display", "display": ":99"})
        self.assertIs(window_kwargs["parse_coordinate_pair_fn"], bindings["parse_coordinate_pair"])


if __name__ == "__main__":
    unittest.main()
