#!/usr/bin/env python3
"""No-network tests for local-ci Linux desktop action execution helpers."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("linux_desktop_action.py")


class LinuxDesktopActionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_fetch_ssh_artifact_creates_parent_and_handles_optional_failures(self) -> None:
        copied = self.root / "artifacts" / "remote.txt"

        def successful_copy(_command, **_kwargs):
            copied.write_text("payload")
            return subprocess.CompletedProcess([], 0, stdout="", stderr="")

        self.assertTrue(self.mod.fetch_ssh_artifact("host", "/tmp/remote.txt", copied, run_fn=successful_copy))
        self.assertEqual(copied.read_text(), "payload")

        failed = self.root / "artifacts" / "missing.txt"
        failure = lambda *_args, **_kwargs: subprocess.CompletedProcess([], 1, stdout="", stderr="missing")
        self.assertFalse(
            self.mod.fetch_ssh_artifact("host", "/tmp/missing.txt", failed, optional=True, run_fn=failure)
        )
        with self.assertRaisesRegex(RuntimeError, "missing"):
            self.mod.fetch_ssh_artifact("host", "/tmp/missing.txt", failed, run_fn=failure)

    def test_cleanup_remote_ssh_dir_swallows_cleanup_errors(self) -> None:
        calls = []

        def cleanup(host, command, *, timeout):
            calls.append((host, command, timeout))
            raise RuntimeError("ssh unavailable")

        self.mod.cleanup_remote_ssh_dir("host", '"$HOME/bundle"', ssh_command_result_fn=cleanup)
        self.assertEqual(calls, [("host", 'rm -rf "$HOME/bundle"', 20)])

    def test_run_linux_xvfb_remote_action_attaches_source_context_and_manifest(self) -> None:
        bundle_dir = self.root / "bundle"
        bundle_dir.mkdir()
        source_context = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "c" * 40,
            "prepare_command": "./scripts/build-ui-preview.sh",
            "prepare_timeout_secs": 180.0,
            "prepared_root": "$HOME/.local/state/pulp/desktop-source/ubuntu/abc123",
            "prepared_root_display": "~/.local/state/pulp/desktop-source/ubuntu/abc123",
            "launch_cwd": "$HOME/.local/state/pulp/desktop-source/ubuntu/abc123",
            "launch_cwd_display": "~/.local/state/pulp/desktop-source/ubuntu/abc123",
            "launch_command": "$HOME/.local/state/pulp/desktop-source/ubuntu/abc123/build/ui-preview",
            "prepare_log": str(self.root / "prepare.log"),
            "prepared_state": "clean",
        }
        built_commands = []
        fetched = []
        cleaned = []
        rollups = []

        def artifact_paths(current_bundle: Path, _output_path: str | None) -> dict[str, Path]:
            return {
                "screenshot": current_bundle / "screenshots" / "window.png",
                "before_screenshot": current_bundle / "screenshots" / "before.png",
                "diff_screenshot": current_bundle / "screenshots" / "diff.png",
                "ui_snapshot": current_bundle / "ui-tree.json",
                "stdout": current_bundle / "stdout.log",
                "stderr": current_bundle / "stderr.log",
            }

        def fetch_artifact(_host, remote_path, local_path, *, optional=False, timeout=60):
            fetched.append((remote_path, local_path.name, optional))
            local_path.parent.mkdir(parents=True, exist_ok=True)
            if local_path.name == "ui-tree.json":
                local_path.write_text(json.dumps({"id": "root", "type": "Window"}))
            elif local_path.suffix == ".log":
                local_path.write_text(f"log:{Path(remote_path).name}\n")
            else:
                local_path.write_bytes(b"png")
            return True

        manifest = self.mod.run_linux_xvfb_remote_action(
            {"defaults": {}},
            "ubuntu",
            {"adapter": "linux-xvfb", "repo_path": "/repo"},
            "/repo/build/ui-preview",
            action_name="click",
            label="ui-preview",
            output_path=None,
            pulp_app_automation=True,
            capture_ui_snapshot=True,
            click_point=None,
            click_view_id="bypass-toggle",
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=True,
            settle_secs=0.75,
            timeout_secs=5.0,
            source_request={"mode": "exact-sha", "sha": "c" * 40},
            ensure_host_reachable_fn=lambda *_args: "ubuntu-host",
            probe_linux_launch_backend_fn=lambda _host: {"mode": "xvfb", "path": "/usr/bin/xvfb-run"},
            create_desktop_run_bundle_fn=lambda *_args: bundle_dir,
            desktop_action_artifact_paths_fn=artifact_paths,
            desktop_interaction_requested_fn=lambda **_kwargs: True,
            prepare_linux_exact_sha_source_fn=lambda *_args: source_context,
            remote_linux_bundle_relpath_fn=lambda *_args: ".local/state/pulp/desktop-automation/remote/ubuntu/click/bundle",
            build_linux_xvfb_remote_command_fn=lambda *args, **kwargs: built_commands.append((args, kwargs)) or "remote-cmd",
            build_linux_window_driver_remote_command_fn=lambda *_args, **_kwargs: self.fail("unexpected window driver"),
            run_fn=lambda *_args, **_kwargs: subprocess.CompletedProcess(["ssh"], 0, stdout="", stderr=""),
            fetch_ssh_artifact_fn=fetch_artifact,
            cleanup_remote_ssh_dir_fn=lambda host, expr: cleaned.append((host, expr)),
            default_desktop_label_fn=lambda command: Path(command or "").name,
            image_change_summary_fn=lambda *_args, **_kwargs: {"changed": True},
            parse_coordinate_pair_fn=lambda *_args, **_kwargs: self.fail("unexpected coordinate parse"),
            attach_desktop_source_to_manifest_fn=lambda payload, context: payload.setdefault("source", context),
            atomic_write_text_fn=lambda path, text: path.write_text(text),
            write_desktop_run_rollups_fn=lambda *args, **kwargs: rollups.append((args, kwargs)),
            now_iso_fn=lambda: "2026-06-10T00:00:00+00:00",
            view_tree_inspector_summary_fn=lambda tree: {"node_count": 1, "root_type": tree["type"]},
            pulp_app_interaction_summary_fn=lambda **kwargs: {"mode": "pulp-app", "selector": kwargs},
        )

        self.assertEqual(built_commands[0][0][2], source_context["launch_command"])
        self.assertEqual(built_commands[0][1]["launch_cwd"], source_context["launch_cwd"])
        self.assertEqual(manifest["source"], source_context)
        self.assertEqual(manifest["inspector"], {"node_count": 1, "root_type": "Window"})
        self.assertEqual(manifest["interaction"]["mode"], "pulp-app")
        self.assertIn(("ubuntu-host", '"$HOME/.local/state/pulp/desktop-automation/remote/ubuntu/click/bundle"'), cleaned)
        self.assertTrue((bundle_dir / "manifest.json").exists())
        self.assertEqual(len(rollups), 2)
        self.assertTrue(any(item[1] == "ui-tree.json" for item in fetched))


if __name__ == "__main__":
    unittest.main()
