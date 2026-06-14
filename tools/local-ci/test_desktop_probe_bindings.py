#!/usr/bin/env python3
"""Tests for desktop probe facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopProbeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        return {
            "_windows_probe": types.SimpleNamespace(),
            "_desktop_doctor": types.SimpleNamespace(),
            "WINDOWS_REQUIRED_REMOTE_TOOLS": {"git": {"required": True}},
            "WINDOWS_OPTIONAL_REMOTE_TOOLS": {"gh": {"required": False}},
            "sys": types.SimpleNamespace(platform="darwin"),
            "shutil": types.SimpleNamespace(which=object()),
            "urllib": types.SimpleNamespace(
                request=types.SimpleNamespace(Request=object(), urlopen=object()),
            ),
        }

    def test_windows_repo_probe_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ok": True}

        bindings = self._bindings()
        bindings["_windows_probe"].probe_windows_repo_checkout = runner
        for name in [
            "run_windows_ssh_powershell",
            "windows_repo_path_is_unsafe",
            "parse_windows_ssh_json",
            "ps_literal",
        ]:
            bindings[name] = object()

        self.assertEqual(self.mod.probe_windows_repo_checkout(bindings, "win", r"C:\Pulp"), {"ok": True})
        self.assertEqual(captured["args"], ("win", r"C:\Pulp"))
        self.assertIs(captured["kwargs"]["run_windows_ssh_powershell_fn"], bindings["run_windows_ssh_powershell"])
        self.assertIs(captured["kwargs"]["windows_repo_path_is_unsafe_fn"], bindings["windows_repo_path_is_unsafe"])
        self.assertIs(captured["kwargs"]["parse_windows_ssh_json_fn"], bindings["parse_windows_ssh_json"])
        self.assertIs(captured["kwargs"]["ps_literal_fn"], bindings["ps_literal"])

    def test_windows_repo_checkout_ensure_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ready": True}

        bindings = self._bindings()
        bindings["_windows_probe"].ensure_windows_remote_repo_checkout = runner
        for name in [
            "probe_windows_repo_checkout",
            "windows_repo_path_is_unsafe",
            "windows_default_repo_checkout_path",
            "run_windows_ssh_powershell",
            "parse_windows_ssh_json",
            "windows_contract_expand_expression",
            "ps_literal",
        ]:
            bindings[name] = object()

        result = self.mod.ensure_windows_remote_repo_checkout(
            bindings,
            "win",
            r"C:\Pulp",
            remote_url="https://example/repo.git",
            bundle_name="bundle",
            bundle_ref="refs/bundle",
        )

        self.assertEqual(result, {"ready": True})
        self.assertEqual(captured["args"], ("win", r"C:\Pulp"))
        self.assertEqual(captured["kwargs"]["remote_url"], "https://example/repo.git")
        self.assertEqual(captured["kwargs"]["bundle_name"], "bundle")
        self.assertEqual(captured["kwargs"]["bundle_ref"], "refs/bundle")
        for name in [
            "probe_windows_repo_checkout",
            "windows_repo_path_is_unsafe",
            "windows_default_repo_checkout_path",
            "run_windows_ssh_powershell",
            "parse_windows_ssh_json",
            "windows_contract_expand_expression",
            "ps_literal",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_windows_session_tooling_bind_dependencies(self):
        cases = [
            (
                "probe_windows_session_agent",
                self.mod.probe_windows_session_agent,
                ("win", {"task_name": "task"}),
                ["run_windows_ssh_powershell", "parse_windows_ssh_json", "windows_contract_expand_expression", "ps_literal"],
            ),
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

    def test_ensure_windows_remote_tooling_binds_facade_dependencies(self):
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

    def test_desktop_doctor_and_webdriver_bind_dependencies(self):
        captured = {}

        def doctor_runner(*args, **kwargs):
            captured["doctor_args"] = args
            captured["doctor_kwargs"] = kwargs
            return [{"name": "ok"}]

        def webdriver_runner(*args, **kwargs):
            captured["webdriver_args"] = args
            captured["webdriver_kwargs"] = kwargs
            return {"ready": True}

        bindings = self._bindings()
        bindings["_desktop_doctor"].desktop_doctor_checks = doctor_runner
        bindings["_desktop_doctor"].probe_webdriver_endpoint = webdriver_runner
        for name in [
            "resolve_desktop_target",
            "desktop_target_contract",
            "desktop_receipt_for",
            "macos_accessibility_trusted",
            "ssh_reachable",
            "ssh_failure_detail",
            "probe_linux_launch_backend",
            "probe_linux_remote_tooling",
            "probe_windows_session_agent",
            "probe_windows_remote_tooling",
            "probe_windows_repo_checkout",
            "probe_webdriver_endpoint",
        ]:
            bindings[name] = object()

        self.assertEqual(self.mod.desktop_doctor_checks(bindings, {"desktop_automation": {}}, "mac"), [{"name": "ok"}])
        self.assertEqual(captured["doctor_args"], ({"desktop_automation": {}}, "mac"))
        for name in [
            "resolve_desktop_target",
            "desktop_target_contract",
            "desktop_receipt_for",
            "macos_accessibility_trusted",
            "ssh_reachable",
            "ssh_failure_detail",
            "probe_linux_launch_backend",
            "probe_linux_remote_tooling",
            "probe_windows_session_agent",
            "probe_windows_remote_tooling",
            "probe_windows_repo_checkout",
            "probe_webdriver_endpoint",
        ]:
            self.assertIs(captured["doctor_kwargs"][f"{name}_fn"], bindings[name])
        self.assertEqual(captured["doctor_kwargs"]["platform"], "darwin")
        self.assertIs(captured["doctor_kwargs"]["which_fn"], bindings["shutil"].which)

        self.assertEqual(self.mod.probe_webdriver_endpoint(bindings, "http://driver", timeout=1.5), {"ready": True})
        self.assertEqual(captured["webdriver_args"], ("http://driver",))
        self.assertEqual(captured["webdriver_kwargs"]["timeout"], 1.5)
        self.assertIs(captured["webdriver_kwargs"]["request_cls"], bindings["urllib"].request.Request)
        self.assertIs(captured["webdriver_kwargs"]["urlopen_fn"], bindings["urllib"].request.urlopen)

    def test_install_desktop_probe_helpers_wires_named_exports(self):
        captured = {}

        def repo_probe(host, repo_path, **kwargs):
            captured["repo"] = (host, repo_path, kwargs)
            return {"checkout": repo_path}

        def webdriver_probe(base_url, **kwargs):
            captured["webdriver"] = (base_url, kwargs)
            return {"base_url": base_url}

        bindings = self._bindings()
        bindings["_windows_probe"].probe_windows_repo_checkout = repo_probe
        bindings["_desktop_doctor"].probe_webdriver_endpoint = webdriver_probe
        for name in [
            "run_windows_ssh_powershell",
            "windows_repo_path_is_unsafe",
            "parse_windows_ssh_json",
            "ps_literal",
        ]:
            bindings[name] = object()

        self.mod.install_desktop_probe_helpers(
            bindings,
            ("probe_windows_repo_checkout", "probe_webdriver_endpoint"),
        )

        self.assertEqual(bindings["probe_windows_repo_checkout"]("win", r"C:\Pulp"), {"checkout": r"C:\Pulp"})
        self.assertEqual(bindings["probe_webdriver_endpoint"]("http://driver"), {"base_url": "http://driver"})
        self.assertEqual(captured["repo"][0:2], ("win", r"C:\Pulp"))
        self.assertEqual(captured["webdriver"][0], "http://driver")

    def test_desktop_probe_exports_compose_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_WINDOWS_PROBE_EXPORTS,
            *self.mod.DESKTOP_DOCTOR_PROBE_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_install_desktop_probe_helpers_routes_selected_groups(self):
        captured = {}

        def repo_probe(host, repo_path, **kwargs):
            captured["repo"] = (host, repo_path, kwargs)
            return {"checkout": repo_path}

        def webdriver_probe(base_url, **kwargs):
            captured["webdriver"] = (base_url, kwargs)
            return {"base_url": base_url}

        bindings = self._bindings()
        bindings["_windows_probe"].probe_windows_repo_checkout = repo_probe
        bindings["_desktop_doctor"].probe_webdriver_endpoint = webdriver_probe
        for name in [
            "run_windows_ssh_powershell",
            "windows_repo_path_is_unsafe",
            "parse_windows_ssh_json",
            "ps_literal",
        ]:
            bindings[name] = object()

        self.mod.install_desktop_probe_helpers(
            bindings,
            ("probe_windows_repo_checkout", "probe_webdriver_endpoint"),
        )

        self.assertEqual(bindings["probe_windows_repo_checkout"]("win", r"C:\Pulp"), {"checkout": r"C:\Pulp"})
        self.assertEqual(bindings["probe_webdriver_endpoint"]("http://driver"), {"base_url": "http://driver"})
        self.assertNotIn("probe_windows_remote_tooling", bindings)
        self.assertNotIn("desktop_doctor_checks", bindings)

    def test_install_desktop_probe_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_desktop_probe_helper = lambda _bindings: "future"

        self.mod.install_desktop_probe_helpers(bindings, ("future_desktop_probe_helper",))

        self.assertEqual(bindings["future_desktop_probe_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
