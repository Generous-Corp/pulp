#!/usr/bin/env python3
"""No-network tests for desktop reporting CLI line helpers."""

from __future__ import annotations

import pathlib
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("desktop_reporting_lines.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopReportingLinesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

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

    def test_desktop_publish_lines_preserve_report_output(self) -> None:
        report = {
            "run_count": 2,
            "output_dir": "/tmp/publish",
            "index_html": "/tmp/publish/index.html",
            "index_json": "/tmp/publish/index.json",
        }

        self.assertEqual(
            self.mod.desktop_publish_lines(report),
            [
                "Desktop publish report ready:",
                "  runs: 2",
                "  output_dir: /tmp/publish",
                "  index_html: /tmp/publish/index.html",
                "  index_json: /tmp/publish/index.json",
            ],
        )

    def test_desktop_cleanup_lines_preserve_empty_and_truncated_output(self) -> None:
        paths = [pathlib.Path(f"/tmp/bundle-{index}") for index in range(12)]

        self.assertEqual(self.mod.desktop_cleanup_empty_line(), "Desktop cleanup: nothing to remove.")
        self.assertEqual(
            self.mod.desktop_cleanup_lines(paths),
            ["Desktop cleanup removed 12 bundle(s)."] + [f"  /tmp/bundle-{index}" for index in range(10)],
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
