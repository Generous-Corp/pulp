#!/usr/bin/env python3
"""No-network tests for desktop status/config/action CLI line helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_status_lines.py")


class DesktopStatusLinesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_smoke_success_lines_include_artifacts_interaction_and_image_change(self) -> None:
        manifest = {
            "label": "demo",
            "pid": 123,
            "artifacts": {
                "before_screenshot": "/tmp/before.png",
                "diff_screenshot": "/tmp/diff.png",
                "image_change": {
                    "changed": True,
                    "method": "pillow",
                    "bbox": {"left": 1, "top": 2, "right": 3, "bottom": 4},
                },
                "screenshot": "/tmp/window.png",
                "ui_snapshot": "/tmp/ui-tree.json",
                "bundle_dir": "/tmp/bundle",
            },
            "interaction": {
                "mode": "desktop-event",
                "click": {"screen_point": {"x": 10.5, "y": 20.25}},
            },
        }

        self.assertEqual(
            self.mod.desktop_action_success_lines("smoke", "mac", manifest),
            [
                "Desktop smoke PASS for `mac`",
                "  label: demo",
                "  pid: 123",
                "  before_screenshot: /tmp/before.png",
                "  diff_screenshot: /tmp/diff.png",
                "  image_change: changed=True method=pillow",
                "  image_change_bbox: 1,2 -> 3,4",
                "  screenshot: /tmp/window.png",
                "  ui_snapshot: /tmp/ui-tree.json",
                "  interaction_mode: desktop-event",
                "  click_screen_point: 10.5,20.25",
                "  bundle: /tmp/bundle",
            ],
        )

    def test_click_success_lines_skip_optional_values_when_absent(self) -> None:
        manifest = {
            "label": "demo",
            "pid": None,
            "artifacts": {
                "screenshot": "/tmp/window.png",
                "bundle_dir": "/tmp/bundle",
            },
            "interaction": {"click": {"screen_point": {}}},
        }

        self.assertEqual(
            self.mod.desktop_action_success_lines("click", "windows", manifest),
            [
                "Desktop click PASS for `windows`",
                "  label: demo",
                "  pid: None",
                "  screenshot: /tmp/window.png",
                "  bundle: /tmp/bundle",
            ],
        )

    def test_inspect_success_lines_keep_existing_short_shape(self) -> None:
        manifest = {
            "label": "inspect-demo",
            "pid": 456,
            "artifacts": {
                "before_screenshot": "/tmp/before.png",
                "diff_screenshot": "/tmp/diff.png",
                "image_change": {"changed": True, "method": "hash"},
                "screenshot": "/tmp/window.png",
                "ui_snapshot": "/tmp/ui-tree.json",
                "bundle_dir": "/tmp/bundle",
            },
            "interaction": {
                "mode": "desktop-event",
                "click": {"screen_point": {"x": 1, "y": 2}},
            },
        }

        self.assertEqual(
            self.mod.desktop_action_success_lines("inspect", "ubuntu", manifest),
            [
                "Desktop inspect PASS for `ubuntu`",
                "  label: inspect-demo",
                "  pid: 456",
                "  screenshot: /tmp/window.png",
                "  ui_snapshot: /tmp/ui-tree.json",
                "  bundle: /tmp/bundle",
            ],
        )

    def test_desktop_config_show_lines_preserve_summary_output(self) -> None:
        desktop_config = {
            "artifact_root": "/tmp/desktop-artifacts",
            "publish_mode": "branch",
            "publish_branch": "gh-pages",
            "retention_days": 14,
        }

        self.assertEqual(
            self.mod.desktop_config_show_lines(desktop_config),
            [
                "Desktop automation config:",
                "  artifact_root: /tmp/desktop-artifacts",
                "  publish_mode: branch",
                "  publish_branch: gh-pages",
                "  retention_days: 14",
                "  target optional keys: target.<name>.(webview_driver|webdriver_url|debug_attach|debugger_command|video_capture|frame_stats)",
            ],
        )

    def test_desktop_config_update_lines_preserve_config_path_output(self) -> None:
        payload = {
            "key": "retention_days",
            "value": 7,
            "config_path": "/tmp/local-ci/config.json",
        }

        self.assertEqual(
            self.mod.desktop_config_update_lines(payload),
            [
                "Desktop automation config updated: retention_days = 7",
                "  config: /tmp/local-ci/config.json",
            ],
        )

    def test_desktop_status_lines_preserve_target_publish_and_proof_output(self) -> None:
        desktop_config = {
            "artifact_root": "/tmp/desktop-artifacts",
            "publish_mode": "branch",
            "publish_branch": "desktop-proofs",
            "retention_days": 30,
        }
        latest_publish = {
            "label": "nightly",
            "generated_at": "2026-06-10T20:00:00Z",
            "output_dir": "/tmp/publish",
            "index_html": "/tmp/publish/index.html",
        }
        target_payloads = [
            {
                "name": "windows",
                "enabled": True,
                "adapter": "windows-session-agent",
                "bootstrap": "scheduled-task",
                "type": "ssh",
                "host": "win-host",
                "repo_path": r"C:\Pulp",
                "capability_tier": "v2",
                "capabilities_text": "launch_app, wait_ready",
                "optional_capabilities": ["webview_dom", "debug_attach"],
                "optional_features": {"debug_attach": True, "webview_driver": False},
                "installed": True,
                "installed_at": "2026-06-10T19:00:00Z",
                "remote_bootstrap_ready": True,
                "remote_tooling_ready": True,
                "remote_repo_checkout_ready": True,
                "contract": {"task_name": "PulpDesktopAgent", "remote_root": r"C:\Users\daniel\PulpAgent"},
                "tooling_probe": {"git_found": True, "gh_found": True},
                "repo_checkout_probe": {"repo_path": r"C:\Pulp"},
                "latest_run": {
                    "label": "failed-run",
                    "completed_at": "2026-06-10T19:30:00Z",
                    "run_status": "error",
                    "source_mode": "exact-sha",
                    "source_sha": "abcdef1234567890",
                    "source_branch": "feature/demo",
                    "host": "win-host",
                    "proof_scope": "live-host",
                    "interaction_mode": "pulp-app",
                    "before_screenshot": "/tmp/before.png",
                    "diff_screenshot": "/tmp/diff.png",
                    "image_change": {"changed": True, "method": "pillow"},
                    "screenshot": "/tmp/window.png",
                    "ui_snapshot": "/tmp/ui-tree.json",
                    "bundle_dir": "/tmp/bundle",
                },
                "latest_proof": {
                    "action": "inspect",
                    "source": {"mode": "exact-sha", "sha": "fedcba9876543210"},
                    "latest_run": {
                        "completed_at": "2026-06-10T19:20:00Z",
                        "artifacts": {"bundle_dir": "/tmp/proof-bundle"},
                    },
                    "proof_scope": "live-host",
                    "host": "win-host",
                    "run_count": 2,
                },
            }
        ]

        def tooling_detail(probe, tool_name):
            return f"{tool_name}-detail"

        def repo_checkout_detail(probe, *, fallback_path=None):
            return f"{probe['repo_path']} fallback={fallback_path}"

        self.assertEqual(
            self.mod.desktop_status_lines(
                desktop_config,
                target_payloads,
                latest_publish=latest_publish,
                short_sha_fn=lambda value: value[:7],
                windows_tooling_detail_fn=tooling_detail,
                windows_repo_checkout_detail_fn=repo_checkout_detail,
            ),
            [
                "Desktop automation:",
                "  artifact_root: /tmp/desktop-artifacts",
                "  publish_mode: branch",
                "  publish_branch: desktop-proofs",
                "  retention_days: 30",
                "  latest_publish: nightly @ 2026-06-10T20:00:00Z",
                "  latest_publish_dir: /tmp/publish",
                "  latest_publish_html: /tmp/publish/index.html",
                "",
                "Targets:",
                "  windows:",
                "    enabled: True",
                "    adapter: windows-session-agent",
                "    bootstrap: scheduled-task",
                "    type: ssh",
                "    host: win-host",
                r"    repo_path: C:\Pulp",
                "    capability_tier: v2",
                "    capabilities: launch_app, wait_ready",
                "    optional_capabilities: webview_dom, debug_attach",
                '    optional_features: {"debug_attach": true, "webview_driver": false}',
                "    installed: yes",
                "    installed_at: 2026-06-10T19:00:00Z",
                "    remote_bootstrap_ready: True",
                "    remote_tooling_ready: True",
                "    remote_repo_checkout_ready: True",
                "    task_name: PulpDesktopAgent",
                r"    remote_root: C:\Users\daniel\PulpAgent",
                "    remote_git: git-detail",
                "    remote_gh: gh-detail",
                r"    remote_repo_checkout: C:\Pulp fallback=C:\Pulp",
                "    latest_run: failed-run @ 2026-06-10T19:30:00Z",
                "    latest_run_status: error",
                "    latest_run_source: mode=exact-sha sha=abcdef1 branch=feature/demo",
                "    latest_run_host: win-host",
                "    latest_run_proof_scope: live-host",
                "    latest_interaction_mode: pulp-app",
                "    latest_before_screenshot: /tmp/before.png",
                "    latest_diff_screenshot: /tmp/diff.png",
                "    latest_image_change: changed=True method=pillow",
                "    latest_screenshot: /tmp/window.png",
                "    latest_ui_snapshot: /tmp/ui-tree.json",
                "    latest_bundle: /tmp/bundle",
                "    latest_proof: inspect mode=exact-sha sha=fedcba9 @ 2026-06-10T19:20:00Z",
                "    latest_proof_scope: live-host host=win-host runs=2",
                "    latest_proof_bundle: /tmp/proof-bundle",
            ],
        )


if __name__ == "__main__":
    unittest.main()
