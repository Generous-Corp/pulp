#!/usr/bin/env python3
"""No-network tests for Windows desktop action result helpers."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("windows_desktop_action_result.py")


class WindowsDesktopActionResultTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.paths = {
            "screenshot_path": self.root / "screenshots" / "window.png",
            "before_screenshot_path": self.root / "screenshots" / "before.png",
            "diff_screenshot_path": self.root / "screenshots" / "diff.png",
            "ui_snapshot_path": self.root / "ui-tree.json",
            "log_path": self.root / "stdout.log",
            "err_path": self.root / "stderr.log",
        }
        self.request = {
            "job_id": "job-123",
            "outputs": {
                "result_root": r"C:\Agent\results\job-123",
                "manifest": r"C:\Agent\results\job-123\manifest.json",
                "stdout": r"C:\Agent\results\job-123\stdout.log",
                "stderr": r"C:\Agent\results\job-123\stderr.log",
                "screenshot": r"C:\Agent\results\job-123\screenshots\window.png",
                "before_screenshot": r"C:\Agent\results\job-123\screenshots\before.png",
                "ui_snapshot": r"C:\Agent\results\job-123\ui-tree.json",
            },
        }

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_wait_for_manifest_polls_until_available(self) -> None:
        ticks = iter([0.0, 0.0, 0.5, 1.0])
        reads = []

        def read_json(_host, remote_path, **kwargs):
            reads.append((remote_path, kwargs))
            if len(reads) < 2:
                return None
            return {"status": "pass"}

        manifest = self.mod.wait_for_windows_session_agent_manifest(
            host="win-host",
            target_name="windows",
            request=self.request,
            timeout_secs=5.0,
            settle_secs=0.25,
            time_fn=lambda: next(ticks),
            sleep_fn=lambda _secs: None,
            windows_ssh_read_json_fn=read_json,
        )

        self.assertEqual(manifest, {"status": "pass"})
        self.assertEqual(len(reads), 2)
        self.assertEqual(reads[0][1], {"timeout": 15, "optional": True})

    def test_wait_for_manifest_times_out(self) -> None:
        times = iter([0.0, 20.0])
        with self.assertRaisesRegex(RuntimeError, "Timed out waiting"):
            self.mod.wait_for_windows_session_agent_manifest(
                host="win-host",
                target_name="windows",
                request=self.request,
                timeout_secs=1.0,
                settle_secs=0.0,
                time_fn=lambda: next(times),
                sleep_fn=lambda _secs: None,
                windows_ssh_read_json_fn=lambda *_args, **_kwargs: None,
            )

    def test_fetch_outputs_writes_missing_optional_logs(self) -> None:
        fetched = []

        def fetch(_host, remote_path, local_path, **kwargs):
            fetched.append((remote_path, local_path.name, kwargs))
            local_path.parent.mkdir(parents=True, exist_ok=True)
            if local_path.suffix == ".log":
                return False
            if local_path.name == "ui-tree.json":
                local_path.write_text("{}")
            else:
                local_path.write_bytes(b"png")
            return True

        self.mod.fetch_windows_session_agent_outputs(
            host="win-host",
            request=self.request,
            capture_before=True,
            capture_ui_snapshot=True,
            screenshot_path=self.paths["screenshot_path"],
            before_screenshot_path=self.paths["before_screenshot_path"],
            ui_snapshot_path=self.paths["ui_snapshot_path"],
            log_path=self.paths["log_path"],
            err_path=self.paths["err_path"],
            windows_ssh_fetch_file_fn=fetch,
        )

        self.assertEqual(self.paths["log_path"].read_text(), "")
        self.assertEqual(self.paths["err_path"].read_text(), "")
        self.assertTrue(self.paths["screenshot_path"].exists())
        self.assertTrue(self.paths["before_screenshot_path"].exists())
        self.assertTrue(self.paths["ui_snapshot_path"].exists())
        self.assertEqual(fetched[0][2], {"optional": True, "timeout": 30})

    def test_build_manifest_adds_artifacts_inspector_and_interaction(self) -> None:
        for path in [self.paths["screenshot_path"], self.paths["before_screenshot_path"]]:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"png")
        self.paths["ui_snapshot_path"].write_text(json.dumps({"id": "root", "type": "Window"}))

        def image_change(_before_path, _after_path, *, diff_output_path=None):
            diff_output_path.write_bytes(b"diff")
            return {"changed": True}

        manifest = self.mod.build_windows_desktop_action_manifest(
            target_name="windows",
            target={"adapter": "windows-session-agent", "repo_path": r"C:\Pulp"},
            command=r"C:\Pulp\build\ui-preview.exe",
            launch_command=r"C:\Prepared\ui-preview.exe",
            host="win-host",
            action_name="inspect",
            label=None,
            started_at="2026-06-11T00:00:00+00:00",
            completed_at="2026-06-11T00:00:01+00:00",
            remote_manifest={"status": "pass", "pid": 5153, "window": {"title": "UI Preview"}},
            bundle_dir=self.root,
            agent_manifest_path=self.root / "agent-manifest.json",
            capture_before=True,
            capture_ui_snapshot=True,
            interaction_requested=True,
            pulp_app_automation=True,
            click_point=None,
            click_view_id="bypass",
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            default_desktop_label_fn=lambda command: Path(command or "").name,
            image_change_summary_fn=image_change,
            view_tree_inspector_summary_fn=lambda tree: {"root_type": tree["type"]},
            pulp_app_interaction_summary_fn=lambda **selector: {"mode": "pulp-app", "selector": selector},
            **self.paths,
        )

        self.assertEqual(manifest["label"], "C:\\Pulp\\build\\ui-preview.exe")
        self.assertEqual(manifest["command"], r"C:\Prepared\ui-preview.exe")
        self.assertEqual(manifest["agent_status"], "pass")
        self.assertEqual(manifest["artifacts"]["image_change"], {"changed": True})
        self.assertTrue(manifest["artifacts"]["diff_screenshot"].endswith("/screenshots/diff.png"))
        self.assertEqual(manifest["inspector"], {"root_type": "Window"})
        self.assertEqual(manifest["interaction"]["selector"]["click_view_id"], "bypass")

    def test_build_manifest_prefers_remote_interaction_and_marks_generic_fallback(self) -> None:
        manifest = self.mod.build_windows_desktop_action_manifest(
            target_name="windows",
            target={"adapter": "windows-session-agent", "repo_path": r"C:\Pulp"},
            command=r"C:\Pulp\calc.exe",
            launch_command=r"C:\Pulp\calc.exe",
            host="win-host",
            action_name="click",
            label="calculator",
            started_at="start",
            completed_at="done",
            remote_manifest={"status": "pass", "interaction": {"mode": "window-capture", "click": {"point": "1,2"}}},
            bundle_dir=self.root,
            agent_manifest_path=self.root / "agent-manifest.json",
            capture_before=False,
            capture_ui_snapshot=False,
            interaction_requested=True,
            pulp_app_automation=False,
            click_point="1,2",
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            default_desktop_label_fn=lambda command: Path(command or "").name,
            image_change_summary_fn=lambda *_args, **_kwargs: {},
            view_tree_inspector_summary_fn=lambda _tree: {},
            pulp_app_interaction_summary_fn=lambda **selector: {"mode": "pulp-app", "selector": selector},
            **self.paths,
        )

        self.assertEqual(manifest["interaction"], {"mode": "window-capture", "click": {"point": "1,2"}})

        fallback = self.mod.build_windows_desktop_action_manifest(
            target_name="windows",
            target={"adapter": "windows-session-agent", "repo_path": r"C:\Pulp"},
            command=r"C:\Pulp\calc.exe",
            launch_command=r"C:\Pulp\calc.exe",
            host="win-host",
            action_name="click",
            label="calculator",
            started_at="start",
            completed_at="done",
            remote_manifest={"status": "pass"},
            bundle_dir=self.root,
            agent_manifest_path=self.root / "agent-manifest.json",
            capture_before=False,
            capture_ui_snapshot=False,
            interaction_requested=True,
            pulp_app_automation=False,
            click_point="1,2",
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            default_desktop_label_fn=lambda command: Path(command or "").name,
            image_change_summary_fn=lambda *_args, **_kwargs: {},
            view_tree_inspector_summary_fn=lambda _tree: {},
            pulp_app_interaction_summary_fn=lambda **selector: {"mode": "pulp-app", "selector": selector},
            **self.paths,
        )

        self.assertEqual(fallback["interaction"]["mode"], "window-capture")


if __name__ == "__main__":
    unittest.main()
