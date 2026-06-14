#!/usr/bin/env python3
"""Tests for Linux desktop action facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("linux_desktop_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxDesktopBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_linux_desktop_exports_include_action_exports(self):
        self.assertEqual(
            self.mod.LINUX_DESKTOP_EXPORTS,
            (
                *self.mod.LINUX_DESKTOP_ARTIFACT_EXPORTS,
                *self.mod.LINUX_DESKTOP_ACTION_EXPORTS,
            ),
        )
        self.assertEqual(len(self.mod.LINUX_DESKTOP_EXPORTS), len(set(self.mod.LINUX_DESKTOP_EXPORTS)))

    def test_run_linux_xvfb_remote_action_binds_facade_dependencies(self):
        captured = {}

        def action_runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ok": True}

        desktop_actions = types.SimpleNamespace(
            desktop_action_artifact_paths=object(),
            desktop_interaction_requested=object(),
            view_tree_inspector_summary=object(),
            pulp_app_interaction_summary=object(),
        )
        subprocess_mod = types.SimpleNamespace(run=object())
        bindings = {
            "_desktop_actions": desktop_actions,
            "_linux_desktop_action": types.SimpleNamespace(run_linux_xvfb_remote_action=action_runner),
            "subprocess": subprocess_mod,
        }
        for name in [
            "ensure_host_reachable",
            "probe_linux_launch_backend",
            "create_desktop_run_bundle",
            "prepare_linux_exact_sha_source",
            "remote_linux_bundle_relpath",
            "build_linux_xvfb_remote_command",
            "build_linux_window_driver_remote_command",
            "fetch_ssh_artifact",
            "cleanup_remote_ssh_dir",
            "default_desktop_label",
            "image_change_summary",
            "parse_coordinate_pair",
            "attach_desktop_source_to_manifest",
            "atomic_write_text",
            "write_desktop_run_rollups",
            "now_iso",
        ]:
            bindings[name] = object()

        result = self.mod.run_linux_xvfb_remote_action(
            bindings,
            {"defaults": {}},
            "ubuntu",
            {"adapter": "linux-xvfb"},
            "./tool",
            action_name="click",
            label="demo",
            output_path=None,
            pulp_app_automation=True,
            capture_ui_snapshot=True,
            click_point=None,
            click_view_id="button",
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=True,
            settle_secs=0.5,
            timeout_secs=2.0,
            source_request={"mode": "exact-sha"},
        )

        self.assertEqual(result, {"ok": True})
        self.assertEqual(captured["args"], ({"defaults": {}}, "ubuntu", {"adapter": "linux-xvfb"}, "./tool"))
        self.assertEqual(captured["kwargs"]["action_name"], "click")
        self.assertIs(captured["kwargs"]["ensure_host_reachable_fn"], bindings["ensure_host_reachable"])
        self.assertIs(captured["kwargs"]["probe_linux_launch_backend_fn"], bindings["probe_linux_launch_backend"])
        self.assertIs(captured["kwargs"]["create_desktop_run_bundle_fn"], bindings["create_desktop_run_bundle"])
        self.assertIs(captured["kwargs"]["desktop_action_artifact_paths_fn"], desktop_actions.desktop_action_artifact_paths)
        self.assertIs(captured["kwargs"]["desktop_interaction_requested_fn"], desktop_actions.desktop_interaction_requested)
        self.assertIs(captured["kwargs"]["prepare_linux_exact_sha_source_fn"], bindings["prepare_linux_exact_sha_source"])
        self.assertIs(captured["kwargs"]["build_linux_xvfb_remote_command_fn"], bindings["build_linux_xvfb_remote_command"])
        self.assertIs(captured["kwargs"]["build_linux_window_driver_remote_command_fn"], bindings["build_linux_window_driver_remote_command"])
        self.assertIs(captured["kwargs"]["run_fn"], subprocess_mod.run)
        self.assertIs(captured["kwargs"]["fetch_ssh_artifact_fn"], bindings["fetch_ssh_artifact"])
        self.assertIs(captured["kwargs"]["cleanup_remote_ssh_dir_fn"], bindings["cleanup_remote_ssh_dir"])
        self.assertIs(captured["kwargs"]["write_desktop_run_rollups_fn"], bindings["write_desktop_run_rollups"])
        self.assertIs(captured["kwargs"]["view_tree_inspector_summary_fn"], desktop_actions.view_tree_inspector_summary)

    def test_ssh_artifact_helpers_bind_facade_dependencies(self):
        captured = {}

        def fetch(*args, **kwargs):
            captured["fetch"] = (args, kwargs)
            return True

        def cleanup(*args, **kwargs):
            captured["cleanup"] = (args, kwargs)

        subprocess_mod = types.SimpleNamespace(run=object())
        ssh_command_result = object()
        bindings = {
            "_linux_desktop_action": types.SimpleNamespace(
                fetch_ssh_artifact=fetch,
                cleanup_remote_ssh_dir=cleanup,
            ),
            "subprocess": subprocess_mod,
            "ssh_command_result": ssh_command_result,
        }
        local_path = Path("/tmp/out.txt")

        self.assertTrue(
            self.mod.fetch_ssh_artifact(
                bindings,
                "host",
                "/tmp/remote.txt",
                local_path,
                optional=True,
                timeout=5,
            )
        )
        self.assertEqual(captured["fetch"][0], ("host", "/tmp/remote.txt", local_path))
        self.assertEqual(captured["fetch"][1]["optional"], True)
        self.assertEqual(captured["fetch"][1]["timeout"], 5)
        self.assertIs(captured["fetch"][1]["run_fn"], subprocess_mod.run)

        self.mod.cleanup_remote_ssh_dir(bindings, "host", '"$HOME/bundle"')
        self.assertEqual(captured["cleanup"][0], ("host", '"$HOME/bundle"'))
        self.assertIs(captured["cleanup"][1]["ssh_command_result_fn"], ssh_command_result)

    def test_install_linux_desktop_helpers_wires_named_exports(self):
        captured = {}

        def fetch(*args, **kwargs):
            captured["fetch"] = (args, kwargs)
            return True

        subprocess_mod = types.SimpleNamespace(run=object())
        bindings = {
            "_linux_desktop_action": types.SimpleNamespace(fetch_ssh_artifact=fetch),
            "subprocess": subprocess_mod,
        }

        self.mod.install_linux_desktop_helpers(bindings, ("fetch_ssh_artifact",))

        self.assertTrue(bindings["fetch_ssh_artifact"]("host", "/remote", Path("/local"), timeout=7))
        self.assertEqual(captured["fetch"][0], ("host", "/remote", Path("/local")))
        self.assertEqual(captured["fetch"][1]["timeout"], 7)
        self.assertIs(captured["fetch"][1]["run_fn"], subprocess_mod.run)


if __name__ == "__main__":
    unittest.main()
