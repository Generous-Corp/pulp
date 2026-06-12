#!/usr/bin/env python3
"""No-network tests for local-ci Windows desktop action execution helpers."""

from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_desktop_action.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsDesktopActionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.bundle_dir = self.root / "bundle"
        self.bundle_dir.mkdir()
        self.target = {"adapter": "windows-session-agent", "repo_path": r"C:\Pulp"}
        self.contract = {
            "task_name": r"\Pulp\DesktopAutomationAgent",
            "jobs_dir": r"C:\Agent\jobs",
            "results_dir": r"C:\Agent\results",
        }
        self.probe = {
            "task_present": True,
            "agent_root_exists": True,
            "jobs_dir_exists": True,
            "results_dir_exists": True,
            "script_exists": True,
            "interactive_user": r"DESKTOP\daniel",
        }

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def artifact_paths(self, current_bundle: Path, _output_path: str | None) -> dict[str, Path]:
        return {
            "screenshot": current_bundle / "screenshots" / "window.png",
            "before_screenshot": current_bundle / "screenshots" / "before.png",
            "diff_screenshot": current_bundle / "screenshots" / "diff.png",
            "ui_snapshot": current_bundle / "ui-tree.json",
            "stdout": current_bundle / "stdout.log",
            "stderr": current_bundle / "stderr.log",
        }

    def request_for(self, job_id: str = "job-123") -> dict:
        result_root = rf"C:\Agent\results\{job_id}"
        return {
            "job_id": job_id,
            "outputs": {
                "result_root": result_root,
                "manifest": rf"{result_root}\manifest.json",
                "stdout": rf"{result_root}\stdout.log",
                "stderr": rf"{result_root}\stderr.log",
                "screenshot": rf"{result_root}\screenshots\window.png",
                "before_screenshot": rf"{result_root}\screenshots\before.png",
                "ui_snapshot": rf"{result_root}\ui-tree.json",
            },
        }

    def run_action(self, **overrides):
        request = overrides.pop("request", self.request_for())
        remote_manifest = overrides.pop(
            "remote_manifest",
            {
                "schema": 1,
                "job_id": request["job_id"],
                "status": "pass",
                "pid": 5153,
                "window": {"title": "UI Preview"},
            },
        )
        source_context = overrides.pop("source_context", None)
        fetched = overrides.pop("fetched", [])
        cleaned = overrides.pop("cleaned", [])
        rollups = overrides.pop("rollups", [])
        built_requests = overrides.pop("built_requests", [])

        def fetch_file(_host, remote_path, local_path, *, optional=False, timeout=60):
            fetched.append((remote_path, local_path.name, optional, timeout))
            local_path.parent.mkdir(parents=True, exist_ok=True)
            if local_path.name == "ui-tree.json":
                local_path.write_text(json.dumps({"id": "root", "type": "Window"}))
            elif local_path.suffix == ".log":
                local_path.write_text(f"log:{Path(remote_path).name}\n")
            else:
                local_path.write_bytes(b"png")
            return True

        def prepare_source(bundle_dir, target_name, host, command, context):
            if source_context is None:
                self.fail("unexpected source preparation")
            return source_context

        def build_request(*args, **kwargs):
            built_requests.append((args, kwargs))
            return request

        def image_change_summary(_before_path, _after_path, *, diff_output_path=None):
            if diff_output_path is not None:
                diff_output_path.parent.mkdir(parents=True, exist_ok=True)
                diff_output_path.write_bytes(b"diff")
            return {"changed": True}

        kwargs = {
            "action_name": "inspect",
            "label": "ui-preview",
            "output_path": None,
            "pulp_app_automation": True,
            "capture_ui_snapshot": True,
            "click_point": None,
            "click_view_id": "bypass-toggle",
            "click_view_type": None,
            "click_view_text": None,
            "click_view_label": None,
            "capture_before": True,
            "settle_secs": 0.75,
            "timeout_secs": 5.0,
            "source_request": None,
            "ensure_host_reachable_fn": lambda *_args: "win-host",
            "desktop_receipt_for_fn": lambda _target_name: {"contract": self.contract},
            "desktop_target_contract_fn": lambda *_args: self.fail("unexpected contract fallback"),
            "probe_windows_session_agent_fn": lambda *_args: self.probe,
            "windows_desktop_session_user_fn": lambda probe: probe.get("interactive_user") or probe.get("logged_on_user") or "",
            "create_desktop_run_bundle_fn": lambda *_args: self.bundle_dir,
            "desktop_action_artifact_paths_fn": self.artifact_paths,
            "desktop_interaction_requested_fn": lambda **_kwargs: True,
            "prepare_windows_exact_sha_source_fn": prepare_source,
            "build_windows_session_agent_request_fn": build_request,
            "windows_path_join_fn": lambda *parts: "\\".join(parts),
            "windows_ssh_write_text_fn": lambda *_args: None,
            "start_windows_session_agent_task_fn": lambda *_args: None,
            "time_fn": lambda: 0.0,
            "sleep_fn": lambda _secs: None,
            "windows_ssh_read_json_fn": lambda *_args, **_kwargs: remote_manifest,
            "atomic_write_text_fn": lambda path, text: path.write_text(text),
            "windows_ssh_fetch_file_fn": fetch_file,
            "windows_ssh_remove_path_fn": lambda host, path: cleaned.append((host, path)),
            "default_desktop_label_fn": lambda command: Path(command or "").name,
            "image_change_summary_fn": image_change_summary,
            "view_tree_inspector_summary_fn": lambda tree: {"node_count": 1, "root_type": tree["type"]},
            "pulp_app_interaction_summary_fn": lambda **kwargs: {"mode": "pulp-app", "selector": kwargs},
            "attach_desktop_source_to_manifest_fn": lambda payload, context: payload.setdefault("source", context) if context else None,
            "write_desktop_run_rollups_fn": lambda *args, **kwargs: rollups.append((args, kwargs)),
            "now_iso_fn": lambda: "2026-06-11T00:00:00+00:00",
        }
        kwargs.update(overrides)

        manifest = self.mod.run_windows_session_agent_action(
            {"defaults": {}},
            "windows",
            self.target,
            r"C:\Pulp\build\ui-preview.exe",
            **kwargs,
        )
        return manifest, built_requests, fetched, cleaned, rollups

    def test_run_windows_session_agent_action_writes_manifest_and_fetches_outputs(self) -> None:
        manifest, built_requests, fetched, cleaned, rollups = self.run_action()

        self.assertEqual(manifest["agent_status"], "pass")
        self.assertEqual(manifest["interaction"]["mode"], "pulp-app")
        self.assertEqual(manifest["inspector"], {"node_count": 1, "root_type": "Window"})
        self.assertTrue(manifest["artifacts"]["diff_screenshot"].endswith("/screenshots/diff.png"))
        self.assertTrue((self.bundle_dir / "agent-manifest.json").exists())
        self.assertTrue((self.bundle_dir / "manifest.json").exists())
        self.assertEqual(built_requests[0][0][2], r"C:\Pulp\build\ui-preview.exe")
        self.assertTrue(any(item[1] == "ui-tree.json" for item in fetched))
        self.assertEqual(len(cleaned), 2)
        self.assertEqual(len(rollups), 2)

    def test_run_windows_session_agent_action_exact_sha_attaches_source_and_launch_cwd(self) -> None:
        source_context = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "d" * 40,
            "launch_cwd": r"$env:LOCALAPPDATA\Pulp\desktop-source\windows\abc123",
            "launch_command": r"$env:LOCALAPPDATA\Pulp\desktop-source\windows\abc123\build\ui-preview.exe",
            "prepare_log": str(self.root / "prepare.log"),
        }

        manifest, built_requests, _fetched, _cleaned, _rollups = self.run_action(
            source_request={"mode": "exact-sha", "sha": "d" * 40},
            source_context=source_context,
        )

        self.assertEqual(built_requests[0][0][2], source_context["launch_command"])
        self.assertEqual(built_requests[0][1]["repo_path"], source_context["launch_cwd"])
        self.assertEqual(manifest["source"], source_context)

    def test_run_windows_session_agent_action_supports_generic_window_capture(self) -> None:
        remote_manifest = {
            "schema": 1,
            "job_id": "job-generic",
            "status": "pass",
            "pid": 6161,
            "window": {"title": "Calculator"},
            "interaction": {"mode": "window-capture", "click": {"screen_point": {"x": 80, "y": 120}}},
        }

        manifest, _built_requests, _fetched, _cleaned, _rollups = self.run_action(
            action_name="click",
            label="calculator",
            pulp_app_automation=False,
            capture_ui_snapshot=False,
            click_point="70,256",
            click_view_id=None,
            capture_before=True,
            remote_manifest=remote_manifest,
        )

        self.assertEqual(manifest["agent_status"], "pass")
        self.assertEqual(manifest["interaction"]["mode"], "window-capture")
        self.assertEqual(manifest["window"]["title"], "Calculator")
        self.assertNotIn("inspector", manifest)

    def test_run_windows_session_agent_action_cleans_up_failed_agent_result(self) -> None:
        cleaned = []
        remote_manifest = {
            "schema": 1,
            "job_id": "job-fail",
            "status": "fail",
            "error": "window not found",
        }

        with self.assertRaisesRegex(RuntimeError, "window not found"):
            self.run_action(
                remote_manifest=remote_manifest,
                cleaned=cleaned,
                capture_ui_snapshot=False,
                click_view_id=None,
            )

        self.assertEqual(len(cleaned), 2)
        self.assertTrue((self.bundle_dir / "agent-manifest.json").exists())


if __name__ == "__main__":
    unittest.main()
