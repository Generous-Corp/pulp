#!/usr/bin/env python3
"""Tests for desktop Windows remote tooling dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_windows_remote_tooling_probe_bindings.py")


class DesktopWindowsRemoteToolingProbeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        return {
            "_windows_probe": types.SimpleNamespace(),
            "WINDOWS_REQUIRED_REMOTE_TOOLS": {"git": {"required": True}},
            "WINDOWS_OPTIONAL_REMOTE_TOOLS": {"gh": {"required": False}},
        }

    def test_remote_tooling_exports_match_wrappers(self):
        expected = (
            "probe_windows_remote_tooling",
            "install_windows_remote_tool",
            "ensure_windows_remote_tooling",
        )

        self.assertEqual(self.mod.DESKTOP_WINDOWS_REMOTE_TOOLING_PROBE_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_remote_tooling_probe_and_install_bind_dependencies(self):
        cases = [
            (
                "probe_windows_remote_tooling",
                self.mod.probe_windows_remote_tooling,
                ("win",),
                ["run_windows_ssh_powershell", "parse_windows_ssh_json"],
            ),
            (
                "install_windows_remote_tool",
                self.mod.install_windows_remote_tool,
                ("win", "Git.Git"),
                ["run_windows_ssh_powershell", "ps_literal"],
            ),
        ]
        for runner_name, wrapper, args, dependency_names in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*runner_args, **kwargs):
                    captured["args"] = runner_args
                    captured["kwargs"] = kwargs
                    return {"ok": True}

                bindings = self._bindings()
                setattr(bindings["_windows_probe"], runner_name, runner)
                for name in dependency_names:
                    bindings[name] = object()

                self.assertEqual(wrapper(bindings, *args), {"ok": True})
                self.assertEqual(captured["args"], args)
                for name in dependency_names:
                    self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_ensure_remote_tooling_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"installed": ["git"]}

        bindings = self._bindings()
        bindings["_windows_probe"].ensure_windows_remote_tooling = runner
        bindings["probe_windows_remote_tooling"] = object()
        bindings["install_windows_remote_tool"] = object()

        self.assertEqual(
            self.mod.ensure_windows_remote_tooling(bindings, "win", install_optional=True),
            {"installed": ["git"]},
        )
        self.assertEqual(captured["args"], ("win",))
        self.assertTrue(captured["kwargs"]["install_optional"])
        self.assertIs(captured["kwargs"]["required_tools"], bindings["WINDOWS_REQUIRED_REMOTE_TOOLS"])
        self.assertIs(captured["kwargs"]["optional_tools"], bindings["WINDOWS_OPTIONAL_REMOTE_TOOLS"])
        self.assertIs(captured["kwargs"]["probe_windows_remote_tooling_fn"], bindings["probe_windows_remote_tooling"])
        self.assertIs(captured["kwargs"]["install_windows_remote_tool_fn"], bindings["install_windows_remote_tool"])

    def test_remote_tooling_installer_wires_named_export(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["tooling"] = (args, kwargs)
            return {"ok": True}

        bindings = self._bindings()
        bindings["_windows_probe"].probe_windows_remote_tooling = runner
        bindings["run_windows_ssh_powershell"] = object()
        bindings["parse_windows_ssh_json"] = object()

        self.mod.install_desktop_windows_remote_tooling_probe_helpers(bindings, ("probe_windows_remote_tooling",))

        self.assertEqual(bindings["probe_windows_remote_tooling"]("win"), {"ok": True})
        self.assertNotIn("install_windows_remote_tool", bindings)
        self.assertEqual(captured["tooling"][0], ("win",))


if __name__ == "__main__":
    unittest.main()
