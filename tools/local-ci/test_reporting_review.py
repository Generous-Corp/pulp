#!/usr/bin/env python3
"""No-network tests for reporting_review (re-home of test_reporting.py review cases)."""

from __future__ import annotations

import json
import pathlib
import tempfile
import unittest


from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("reporting_review.py", add_module_dir=True)


class ReportingReviewTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
        self.config = {
            "desktop_automation": {
                "artifact_root": str(self.root / "desktop-artifacts"),
                "publish_mode": "local",
                "publish_branch": "dev-artifacts",
            }
        }

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_desktop_review_issue_draft_lists_attachments_and_fallbacks(self) -> None:
        package_path = self.root / "report" / "review-package.json"
        package_path.parent.mkdir(parents=True)
        issue_video = package_path.parent / "proof.issue.mp4"
        issue_video.write_bytes(b"issue")
        review_package = {
            "label": "Video Proof",
            "index_html": str(package_path.parent / "index.html"),
            "review_markdown": str(package_path.parent / "review.md"),
            "serve_command": "python3 tools/local-ci/local_ci.py desktop serve /tmp/report --host 0.0.0.0 --port 8765",
            "serve_label": "video-proof",
            "serve_background_command": "python3 tools/local-ci/local_ci.py desktop serve /tmp/report --host 0.0.0.0 --port 8765 --background --label video-proof --json",
            "serve_status_command": "python3 tools/local-ci/local_ci.py desktop serve --status --label video-proof --json",
            "serve_stop_command": "python3 tools/local-ci/local_ci.py desktop serve --stop --label video-proof --json",
            "published_cleanup_command": "python3 tools/local-ci/local_ci.py desktop cleanup --published --older-than-days 14 --keep-last 3 --json",
            "serve_urls": ["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"],
            "serve_verification": {
                "kind": "desktop-proof-serve-verification",
                "status": "ok",
                "url": "http://127.0.0.1:8765/",
                "http_status": 200,
            },
            "runs": [
                {
                    "target": "mac",
                    "adapter": "macos-local",
                    "action": "click",
                    "label": "component",
                    "host": "macstudio",
                    "bundle_dir": str(package_path.parent / "runs" / "component"),
                    "command": "./build/pulp --inspect",
                    "source": {"mode": "exact-sha", "branch": "feature/video", "sha": "abc123"},
                    "manifest": {"path": str(package_path.parent / "assets/run/manifest.json"), "relative_path": "assets/run/manifest.json"},
                    "template": "component-zoom",
                    "storyboard": {
                        "title": "Component zoom proof",
                        "steps": [
                            {"index": 1, "label": "Launch", "detail": "mac/click"},
                            {"index": 2, "label": "Action", "detail": "click: bypass-toggle"},
                            {"index": 3, "label": "Review", "detail": "issue attachment ready"},
                        ],
                    },
                    "context": {"component": "bypass-toggle"},
                    "notes": ["Toggle changes state."],
                    "attachment": {
                        "status": "attach-primary",
                        "path": str(issue_video),
                        "relative_path": "assets/proof.issue.mp4",
                        "size_bytes": issue_video.stat().st_size,
                        "budget_bytes": 100000000,
                        "reason": "primary issue MP4 fits the configured attachment budget",
                    },
                },
                {
                    "target": "mac",
                    "action": "smoke",
                    "label": "large-proof",
                    "template": "plugin-host",
                    "attachment": {
                        "status": "fallback-link",
                        "reason": "no issue-ready MP4 fits the configured attachment budget",
                    },
                    "fallback": {
                        "report_path": str(package_path.parent / "index.html"),
                        "review_markdown": str(package_path.parent / "review.md"),
                        "serve_command": "python3 tools/local-ci/local_ci.py desktop serve /tmp/report --host 0.0.0.0 --port 8765",
                        "serve_label": "large-proof",
                        "serve_background_command": "python3 tools/local-ci/local_ci.py desktop serve /tmp/report --host 0.0.0.0 --port 8765 --background --label large-proof --json",
                        "serve_status_command": "python3 tools/local-ci/local_ci.py desktop serve --status --label large-proof --json",
                        "serve_stop_command": "python3 tools/local-ci/local_ci.py desktop serve --stop --label large-proof --json",
                        "published_cleanup_command": "python3 tools/local-ci/local_ci.py desktop cleanup --published --older-than-days 14 --keep-last 3 --json",
                        "serve_urls": ["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"],
                        "serve_verification": {
                            "kind": "desktop-proof-serve-verification",
                            "status": "ok",
                            "url": "http://127.0.0.1:8765/",
                            "http_status": 200,
                        },
                        "internal_ephemeral": True,
                    },
                },
            ],
        }

        draft = self.mod.desktop_review_issue_draft(
            review_package,
            package_path=package_path,
            title="Review proof",
            repo="danielraffel/pulp",
            check_files=True,
        )

        self.assertEqual(draft["kind"], "desktop-video-proof-github-issue-draft")
        self.assertEqual(draft["title"], "Review proof")
        self.assertEqual(len(draft["attachments"]), 1)
        self.assertEqual(draft["attachments"][0]["relative_path"], "assets/proof.issue.mp4")
        self.assertEqual(draft["attachments"][0]["extension"], ".mp4")
        self.assertTrue(draft["attachments"][0]["supported_video_extension"])
        self.assertEqual(draft["attachment_checks"][0]["path"], str(issue_video))
        self.assertTrue(draft["attachment_checks"][0]["fits_attachment_budget"])
        self.assertEqual(draft["attachment_checks"][0]["extension"], ".mp4")
        self.assertTrue(draft["attachment_checks"][0]["supported_video_extension"])
        self.assertEqual(draft["attachment_policy"]["supported_video_extensions"], [".mp4", ".mov", ".webm"])
        self.assertEqual(draft["attachment_policy"]["pro_video_limit_bytes"], 100000000)
        self.assertEqual(draft["attachment_policy"]["free_video_limit_bytes"], 10000000)
        self.assertEqual(draft["serve_urls"], ["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"])
        self.assertEqual(len(draft["fallback_links"]), 1)
        self.assertEqual(draft["fallback_links"][0]["serve_urls"], ["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"])
        self.assertTrue(draft["fallback_links"][0]["serve_verified"])
        self.assertEqual(draft["fallback_links"][0]["serve_verification"]["http_status"], 200)
        self.assertEqual(draft["fallback_links"][0]["serve_label"], "large-proof")
        self.assertIn("--background --label large-proof --json", draft["fallback_links"][0]["serve_background_command"])
        self.assertIn("--status --label large-proof --json", draft["fallback_links"][0]["serve_status_command"])
        self.assertIn("--stop --label large-proof --json", draft["fallback_links"][0]["serve_stop_command"])
        self.assertIn("desktop cleanup --published", draft["fallback_links"][0]["published_cleanup_command"])
        self.assertTrue(draft["fallback_links"][0]["internal_ephemeral"])
        self.assertIn("gh issue create --repo danielraffel/pulp", draft["create_command"])
        self.assertIn("looks good to me", draft["body"])
        self.assertIn("needs work", draft["body"])
        self.assertIn("needs changes", draft["body"])
        self.assertIn("needs another pass", draft["body"])
        self.assertIn("not approved", draft["body"])
        self.assertEqual(
            draft["needs_work_triggers"],
            ["needs work", "needs changes", "needs another pass", "not approved"],
        )
        self.assertIn("Review status command: `python3 tools/local-ci/local_ci.py desktop review-status <issue-url> --repo danielraffel/pulp", draft["body"])
        self.assertIn(f"--manifest {package_path.parent / 'runs' / 'component' / 'manifest.json'} --close-issue", draft["body"])
        self.assertIn("GitHub issue/PR video uploads support .mp4, .mov, .webm", draft["body"])
        self.assertIn("Attach video: `", draft["body"])
        self.assertIn("Command: `./build/pulp --inspect`", draft["body"])
        self.assertIn("Source: `mode=exact-sha, branch=feature/video, sha=abc123`", draft["body"])
        self.assertIn("Host: `macstudio`", draft["body"])
        self.assertIn("Adapter: `macos-local`", draft["body"])
        self.assertIn("Manifest: `", draft["body"])
        self.assertIn("Storyboard:", draft["body"])
        self.assertIn("Launch: mac/click", draft["body"])
        self.assertIn("Action: click: bypass-toggle", draft["body"])
        self.assertIn("Candidate watch URL: `http://100.64.0.10:8765/`", draft["body"])
        self.assertIn("Watch URL verification: `ok` `http://127.0.0.1:8765/`", draft["body"])
        self.assertIn("Served link verification: `ok`", draft["body"])
        self.assertIn("Background serve command: `", draft["body"])
        self.assertIn("--background --label video-proof --json", draft["body"])
        self.assertIn("Status command: `", draft["body"])
        self.assertIn("Stop command: `", draft["body"])
        self.assertIn("Published cleanup command: `", draft["body"])
        self.assertIn("desktop cleanup --published --older-than-days 14 --keep-last 3 --json", draft["body"])
        self.assertIn("Context component: `bypass-toggle`", draft["body"])
        self.assertIn("use the served report link", draft["body"])

    def test_desktop_review_package_quotes_report_paths_with_spaces(self) -> None:
        publish_dir = self.root / "Application Support" / "Pulp" / "runs" / "_published" / "report one"
        package = self.mod.desktop_review_package(
            {
                "generated_at": "2026-06-13T12:00:00+00:00",
                "label": "Report One",
                "publish_mode": "local",
                "publish_branch": "desktop-artifacts",
                "serve_urls": ["http://127.0.0.1:8765/"],
                "runs": [],
            },
            publish_dir=publish_dir,
        )

        self.assertEqual(package["serve_label"], "report-one")
        self.assertIn(f"desktop serve '{publish_dir}' --host", package["serve_command"])
        self.assertIn("--auto-port", package["serve_background_command"])
        self.assertIn("--background --label report-one --json", package["serve_background_command"])
        self.assertIn("--status --label report-one --json", package["serve_status_command"])
        self.assertIn("--stop --label report-one --json", package["serve_stop_command"])

    def test_desktop_review_issue_draft_check_files_rejects_missing_attachment(self) -> None:
        package_path = self.root / "report" / "review-package.json"
        package_path.parent.mkdir(parents=True)
        missing_video = package_path.parent / "missing.issue.mp4"
        review_package = {
            "label": "Video Proof",
            "runs": [
                {
                    "target": "mac",
                    "action": "click",
                    "label": "component",
                    "attachment": {
                        "status": "attach-primary",
                        "path": str(missing_video),
                        "relative_path": "assets/missing.issue.mp4",
                        "size_bytes": 250000,
                        "budget_bytes": 100000000,
                        "reason": "primary issue MP4 fits the configured attachment budget",
                    },
                }
            ],
        }

        with self.assertRaisesRegex(ValueError, "run 1 attachment missing"):
            self.mod.desktop_review_issue_draft(
                review_package,
                package_path=package_path,
                check_files=True,
            )

    def test_desktop_review_issue_draft_check_files_rejects_unverified_fallback(self) -> None:
        package_path = self.root / "report" / "review-package.json"
        package_path.parent.mkdir(parents=True)
        review_package = {
            "label": "Video Proof",
            "serve_urls": ["http://127.0.0.1:8765/"],
            "runs": [
                {
                    "target": "mac",
                    "action": "click",
                    "label": "component",
                    "attachment": {
                        "status": "fallback-link",
                        "reason": "no issue-ready MP4 fits the configured attachment budget",
                    },
                    "fallback": {
                        "serve_urls": ["http://127.0.0.1:8765/"],
                        "serve_background_command": "python3 tools/local-ci/local_ci.py desktop serve /tmp/report --background --label video-proof --json",
                    },
                }
            ],
        }

        with self.assertRaisesRegex(ValueError, "fallback serve URL not verified"):
            self.mod.desktop_review_issue_draft(
                review_package,
                package_path=package_path,
                check_files=True,
            )

    def test_desktop_review_issue_draft_check_files_rejects_over_budget_attachment(self) -> None:
        package_path = self.root / "report" / "review-package.json"
        package_path.parent.mkdir(parents=True)
        issue_video = package_path.parent / "proof.issue.mp4"
        issue_video.write_bytes(b"issue")
        review_package = {
            "label": "Video Proof",
            "runs": [
                {
                    "target": "mac",
                    "action": "click",
                    "label": "component",
                    "attachment": {
                        "status": "attach-primary",
                        "path": str(issue_video),
                        "relative_path": "assets/proof.issue.mp4",
                        "size_bytes": 5,
                        "budget_bytes": 4,
                        "reason": "primary issue MP4 fits the configured attachment budget",
                    },
                }
            ],
        }

        with self.assertRaisesRegex(ValueError, "attachment exceeds budget"):
            self.mod.desktop_review_issue_draft(
                review_package,
                package_path=package_path,
                check_files=True,
            )

    def test_desktop_review_issue_draft_check_files_rejects_unsupported_video_extension(self) -> None:
        package_path = self.root / "report" / "review-package.json"
        package_path.parent.mkdir(parents=True)
        issue_video = package_path.parent / "proof.issue.avi"
        issue_video.write_bytes(b"issue")
        review_package = {
            "label": "Video Proof",
            "runs": [
                {
                    "target": "mac",
                    "action": "click",
                    "label": "component",
                    "attachment": {
                        "status": "attach-primary",
                        "path": str(issue_video),
                        "relative_path": "assets/proof.issue.avi",
                        "size_bytes": issue_video.stat().st_size,
                        "budget_bytes": 100000000,
                        "reason": "primary issue video fits the configured attachment budget",
                    },
                }
            ],
        }

        with self.assertRaisesRegex(ValueError, "unsupported video extension"):
            self.mod.desktop_review_issue_draft(
                review_package,
                package_path=package_path,
                check_files=True,
            )


if __name__ == "__main__":
    unittest.main()
