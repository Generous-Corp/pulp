#!/usr/bin/env python3
"""No-network tests for local-ci desktop CLI line helpers."""

from __future__ import annotations

import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("desktop_cli.py")


def load_module():
    spec = importlib.util.spec_from_file_location("desktop_cli_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class DesktopCliTests(unittest.TestCase):
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

    def test_desktop_recent_lines_preserve_run_summary_output(self) -> None:
        run_summaries = [
            {
                "action": "click",
                "target": "mac",
                "label": "demo-click",
                "completed_at": "2026-06-10T19:00:00Z",
                "run_status": "pass",
                "source": {"mode": "exact-sha", "sha": "abcdef1234567890", "branch": "feature/demo"},
                "proof_scope": "local-window",
                "host": "mac-host",
                "interaction_mode": "desktop-event",
                "artifacts": {
                    "bundle_dir": "/tmp/bundle",
                    "before_screenshot": "/tmp/before.png",
                    "diff_screenshot": "/tmp/diff.png",
                    "image_change": {"changed": True, "method": "pillow"},
                    "screenshot": "/tmp/window.png",
                    "ui_snapshot": "/tmp/ui-tree.json",
                },
            }
        ]

        self.assertEqual(
            self.mod.desktop_recent_lines(run_summaries, short_sha_fn=lambda value: value[:7]),
            [
                "Desktop automation recent runs:",
                "  mac/click: demo-click @ 2026-06-10T19:00:00Z",
                "    status: pass",
                "    source: mode=exact-sha sha=abcdef1 branch=feature/demo",
                "    proof_scope: local-window host=mac-host",
                "    bundle: /tmp/bundle",
                "    before_screenshot: /tmp/before.png",
                "    diff_screenshot: /tmp/diff.png",
                "    interaction_mode: desktop-event",
                "    image_change: changed=True method=pillow",
                "    screenshot: /tmp/window.png",
                "    ui_snapshot: /tmp/ui-tree.json",
            ],
        )

    def test_desktop_recent_lines_use_existing_fallbacks(self) -> None:
        run_summaries = [
            {
                "run_status": "pass",
                "source": {"mode": "direct", "sha": "", "branch": ""},
                "artifacts": {},
            }
        ]

        self.assertEqual(
            self.mod.desktop_recent_lines(run_summaries, short_sha_fn=lambda value: value[:7]),
            [
                "Desktop automation recent runs:",
                "  ?/run: run @ ?",
                "    status: pass",
                "    source: mode=direct sha= branch=?",
                "    bundle: ?",
            ],
        )

    def test_desktop_proof_empty_line_preserves_filter_suffix(self) -> None:
        self.assertEqual(
            self.mod.desktop_proof_empty_line(
                target="mac",
                action="click",
                source_mode="exact-sha",
                sha="abcdef1234567890",
                branch="feature/demo",
                short_sha_fn=lambda value: value[:7],
            ),
            "No desktop proofs found (target=mac, action=click, source_mode=exact-sha, sha=abcdef1, branch=feature/demo).",
        )
        self.assertEqual(
            self.mod.desktop_proof_empty_line(
                target=None,
                action=None,
                source_mode=None,
                sha=None,
                branch=None,
                short_sha_fn=lambda value: value[:7],
            ),
            "No desktop proofs found.",
        )

    def test_desktop_proof_lines_preserve_proof_summary_output(self) -> None:
        proofs = [
            {
                "target": "windows",
                "action": "inspect",
                "proof_scope": "remote-window",
                "adapter": "windows-session-agent",
                "host": "win-host",
                "run_count": 3,
                "source": {"mode": "exact-sha", "sha": "fedcba9876543210", "branch": "feature/demo"},
                "latest_run": {
                    "completed_at": "2026-06-10T19:01:00Z",
                    "label": "inspect-demo",
                    "interaction_mode": "pulp-app",
                    "artifacts": {
                        "bundle_dir": "/tmp/bundle",
                        "screenshot": "/tmp/window.png",
                        "ui_snapshot": "/tmp/ui-tree.json",
                        "agent_manifest": "/tmp/agent-manifest.json",
                    },
                },
            }
        ]

        self.assertEqual(
            self.mod.desktop_proof_lines(proofs, short_sha_fn=lambda value: value[:7]),
            [
                "Desktop automation proofs:",
                "  windows/inspect: mode=exact-sha sha=fedcba9 @ 2026-06-10T19:01:00Z",
                "    proof_scope: remote-window adapter=windows-session-agent host=win-host runs=3",
                "    branch: feature/demo",
                "    label: inspect-demo",
                "    interaction_mode: pulp-app",
                "    bundle: /tmp/bundle",
                "    screenshot: /tmp/window.png",
                "    ui_snapshot: /tmp/ui-tree.json",
                "    agent_manifest: /tmp/agent-manifest.json",
            ],
        )


if __name__ == "__main__":
    unittest.main()
