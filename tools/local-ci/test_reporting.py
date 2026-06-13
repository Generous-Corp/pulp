#!/usr/bin/env python3
"""No-network tests for local-ci desktop report helpers."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import tempfile
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("reporting.py")


def load_module():
    spec = importlib.util.spec_from_file_location("reporting_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class ReportingTests(unittest.TestCase):
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

    def artifact_root(self, config: dict) -> pathlib.Path:
        path = pathlib.Path(config["desktop_automation"]["artifact_root"])
        path.mkdir(parents=True, exist_ok=True)
        return path

    def publish_root(self, config: dict) -> pathlib.Path:
        path = self.artifact_root(config) / "_published"
        path.mkdir(parents=True, exist_ok=True)
        return path

    def atomic_write(self, path: pathlib.Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def test_slugify_token_matches_legacy_edges(self) -> None:
        self.assertEqual(self.mod.slugify_token(" UI Preview / Smoke! "), "ui-preview-smoke")
        self.assertEqual(self.mod.slugify_token("!!!"), "run")
        self.assertEqual(len(self.mod.slugify_token("x" * 80, max_len=12)), 12)

    def test_format_bytes_uses_python39_compatible_type_check(self) -> None:
        self.assertEqual(self.mod._format_bytes("120000"), "unknown")
        self.assertEqual(self.mod._format_bytes(120000), "120 KB")
        self.assertEqual(self.mod._format_bytes(1_200_000), "1.2 MB")

    def test_directory_copy_and_clear_helpers_preserve_git_directory(self) -> None:
        src = self.root / "src"
        dest = self.root / "dest"
        (src / "nested").mkdir(parents=True)
        (src / "file.txt").write_text("file")
        (src / "nested" / "child.txt").write_text("child")
        (dest / ".git").mkdir(parents=True)
        (dest / "old.txt").write_text("old")

        self.mod.copy_directory_contents(src, dest)

        self.assertEqual((dest / "file.txt").read_text(), "file")
        self.assertEqual((dest / "nested" / "child.txt").read_text(), "child")

        self.mod.clear_directory_contents(dest)

        self.assertTrue((dest / ".git").is_dir())
        self.assertFalse((dest / "file.txt").exists())
        self.assertFalse((dest / "nested").exists())
        self.assertFalse((dest / "old.txt").exists())

    def test_stage_desktop_publish_report_rejects_empty_manifest_list(self) -> None:
        with self.assertRaisesRegex(ValueError, "requires at least one run manifest"):
            self.mod.stage_desktop_publish_report(
                self.config,
                [],
                create_desktop_publish_bundle_fn=lambda _config: self.publish_root(self.config) / "unused",
                now_iso_fn=lambda: "2026-05-22T12:01:00+00:00",
                atomic_write_text_fn=self.atomic_write,
                write_desktop_publish_rollups_fn=lambda _config: None,
                publish_report_to_branch_fn=lambda _config, _report: {},
            )

    def test_stage_desktop_publish_report_copies_artifacts_and_writes_index(self) -> None:
        bundle = self.root / "bundle"
        bundle.mkdir()
        stdout_log = bundle / "stdout.log"
        stdout_log.write_text("hello\n")
        video = bundle / "proof.mp4"
        video.write_bytes(b"mp4")
        video_composed = bundle / "proof-composed.mp4"
        video_composed.write_bytes(b"composed")
        video_issue = bundle / "proof.issue.mp4"
        video_issue.write_bytes(b"issue")
        video_small = bundle / "proof.small.mp4"
        video_small.write_bytes(b"small")
        video_metadata = bundle / "metadata.json"
        video_metadata.write_text('{"size":{"fits_attachment_budget":true}}\n')
        video_composed_metadata = bundle / "composed-metadata.json"
        video_composed_metadata.write_text(
            json.dumps(
                {
                    "composer": "remotion",
                    "size": {"size_bytes": 120000, "fits_attachment_budget": True},
                    "review_storyboard": {
                        "title": "Design parity proof",
                        "subtitle": "Design parity proof",
                        "template": "design-parity",
                        "steps": [
                            {"index": 1, "label": "Launch", "detail": "REAPER / PulpSynth / clap"},
                            {"index": 2, "label": "Focus", "detail": "bypass-toggle via id: bypass-toggle"},
                            {"index": 3, "label": "Action", "detail": "click: bypass-toggle / video proof / 4s / 30 fps"},
                            {"index": 4, "label": "Review", "detail": "transcoded (balanced-720p)"},
                        ],
                        "notes": ["Source import matches the native render."],
                    },
                },
                indent=2,
            )
            + "\n"
        )
        video_issue_metadata = bundle / "issue-metadata.json"
        video_issue_metadata.write_text('{"status":"transcoded","selected_attempt":"balanced-720p","size":{"size_bytes":90000,"fits_attachment_budget":true}}\n')
        video_small_metadata = bundle / "small-metadata.json"
        video_small_metadata.write_text('{"status":"transcoded","selected_attempt":"compact-540p","size":{"size_bytes":8000000,"fits_attachment_budget":true}}\n')
        source_reference = bundle / "source-reference.png"
        source_reference.write_bytes(b"png")
        (bundle / "manifest.json").write_text('{"label":"bundle-copy"}\n')
        manifest = {
            "target": "mac<>",
            "adapter": "macos-local",
            "action": "inspect",
            "label": "UI & Smoke",
            "completed_at": "2026-05-22T12:00:00+00:00",
            "host": "macstudio",
            "repo_path": "/repo/pulp",
            "command": ["./build/pulp", "--inspect"],
            "source": {
                "mode": "exact-sha",
                "branch": "feat/validation-video-proof",
                "sha": "abc123",
                "launch_cwd": "/repo/pulp",
            },
            "artifacts": {
                "bundle_dir": str(bundle),
                "stdout": str(stdout_log),
                "video": str(video),
                "video_composed": str(video_composed),
                "video_issue": str(video_issue),
                "video_small": str(video_small),
                "video_metadata": str(video_metadata),
                "video_composed_metadata": str(video_composed_metadata),
                "video_issue_metadata": str(video_issue_metadata),
                "video_small_metadata": str(video_small_metadata),
                "screenshot": str(bundle / "missing.png"),
                "image_change": {"changed": False},
            },
            "interaction": {"mode": "dom"},
            "video_proof_notes": ["Source import matches the native render.", "Critical control remains legible."],
            "video_proof_composition": {
                "template": "design-parity",
                "source_image": str(source_reference),
                "source_label": "Figma source",
                "focus": {
                    "label": "bypass-toggle",
                    "selector": {"click_view_id": "bypass-toggle"},
                    "content_point": {"x": 12.0, "y": 24.0},
                    "normalized_center": {"x": 0.25, "y": 0.5},
                },
                "action_marker": {
                    "kind": "click",
                    "label": "bypass-toggle",
                    "content_point": {"x": 12.0, "y": 24.0},
                    "normalized_point": {"x": 0.25, "y": 0.5},
                },
                "context": {
                    "recipe": "design-parity",
                    "host": "REAPER",
                    "plugin": "PulpSynth",
                    "format": "clap",
                },
                "notes": ["Critical control remains legible."],
            },
        }
        output_dir = self.publish_root(self.config) / "20260522-gallery"
        rollups: list[dict] = []

        report = self.mod.stage_desktop_publish_report(
            self.config,
            [manifest],
            output_dir=output_dir,
            label="Gallery <One>",
            serve_urls=["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"],
            create_desktop_publish_bundle_fn=lambda _config: output_dir,
            now_iso_fn=lambda: "2026-05-22T12:01:00+00:00",
            atomic_write_text_fn=self.atomic_write,
            write_desktop_publish_rollups_fn=lambda config: rollups.append(config),
            publish_report_to_branch_fn=lambda _config, _report: {},
        )

        self.assertEqual(report["label"], "Gallery <One>")
        self.assertEqual(report["run_count"], 1)
        self.assertEqual(report["serve_urls"], ["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"])
        self.assertEqual(len(rollups), 1)
        payload = json.loads((output_dir / "index.json").read_text())
        published_run = payload["runs"][0]
        self.assertEqual(payload["publish_mode"], "local")
        self.assertEqual(payload["serve_urls"], ["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"])
        self.assertEqual(published_run["target"], "mac<>")
        self.assertEqual(published_run["adapter"], "macos-local")
        self.assertEqual(published_run["host"], "macstudio")
        self.assertEqual(published_run["repo_path"], "/repo/pulp")
        self.assertEqual(published_run["command"], "./build/pulp --inspect")
        self.assertEqual(published_run["source"]["mode"], "exact-sha")
        self.assertEqual(published_run["source"]["sha"], "abc123")
        self.assertEqual(published_run["interaction_mode"], "dom")
        self.assertIn("stdout", published_run["artifacts"])
        self.assertIn("video", published_run["artifacts"])
        self.assertIn("video_composed", published_run["artifacts"])
        self.assertIn("video_issue", published_run["artifacts"])
        self.assertIn("video_small", published_run["artifacts"])
        self.assertIn("video_metadata", published_run["artifacts"])
        self.assertIn("video_composed_metadata", published_run["artifacts"])
        self.assertIn("video_issue_metadata", published_run["artifacts"])
        self.assertIn("video_small_metadata", published_run["artifacts"])
        self.assertIn("video_source_image", published_run["artifacts"])
        self.assertIn("manifest", published_run["artifacts"])
        self.assertNotIn("screenshot", published_run["artifacts"])
        self.assertEqual(published_run["artifacts"]["image_change"], {"changed": False})
        self.assertEqual(published_run["video_proof_composition"]["template"], "design-parity")
        self.assertEqual(published_run["video_proof_composition"]["focus"]["label"], "bypass-toggle")
        self.assertEqual(published_run["video_proof_composition"]["action_marker"]["label"], "bypass-toggle")
        self.assertEqual(published_run["video_proof_composition"]["context"]["plugin"], "PulpSynth")
        self.assertEqual(
            published_run["video_proof_notes"],
            ["Source import matches the native render.", "Critical control remains legible."],
        )
        html_text = (output_dir / "index.html").read_text()
        self.assertIn("Gallery &lt;One&gt;", html_text)
        self.assertIn("mac&lt;&gt;/inspect", html_text)
        self.assertIn("<video controls", html_text)
        self.assertIn("video metadata", html_text)
        self.assertIn("template: design-parity", html_text)
        self.assertIn("command: ./build/pulp --inspect", html_text)
        self.assertIn("source: mode=exact-sha, branch=feat/validation-video-proof, sha=abc123", html_text)
        self.assertIn("adapter: macos-local", html_text)
        self.assertIn("host: macstudio", html_text)
        self.assertIn("source: Figma source", html_text)
        self.assertIn("focus: bypass-toggle", html_text)
        self.assertIn("focus_point:", html_text)
        self.assertIn("action: bypass-toggle", html_text)
        self.assertIn("action_point:", html_text)
        self.assertIn("context.plugin: PulpSynth", html_text)
        self.assertIn("context.format: clap", html_text)
        self.assertIn("Launch: REAPER / PulpSynth / clap", html_text)
        self.assertIn("Focus: bypass-toggle via id: bypass-toggle", html_text)
        self.assertIn("Action: click: bypass-toggle / video proof / 4s / 30 fps", html_text)
        self.assertIn("Source import matches the native render.", html_text)
        self.assertIn("source reference", html_text)
        review_text = (output_dir / "review.md").read_text()
        self.assertEqual(report["review_markdown"], str(output_dir / "review.md"))
        review_package = json.loads((output_dir / "review-package.json").read_text())
        self.assertEqual(report["review_package"], str(output_dir / "review-package.json"))
        self.assertEqual(review_package["kind"], "desktop-video-proof-review-package")
        self.assertEqual(review_package["serve_urls"], ["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"])
        self.assertEqual(review_package["runs"][0]["attachment"]["status"], "attach-primary")
        self.assertTrue(review_package["runs"][0]["attachment"]["path"].endswith("proof.issue.mp4"))
        self.assertEqual(review_package["runs"][0]["attachment"]["size_bytes"], 90000)
        self.assertEqual(review_package["runs"][0]["adapter"], "macos-local")
        self.assertEqual(review_package["runs"][0]["host"], "macstudio")
        self.assertEqual(review_package["runs"][0]["repo_path"], "/repo/pulp")
        self.assertEqual(review_package["runs"][0]["command"], "./build/pulp --inspect")
        self.assertEqual(review_package["runs"][0]["source"]["branch"], "feat/validation-video-proof")
        self.assertTrue(review_package["runs"][0]["manifest"]["path"].endswith("manifest.json"))
        self.assertEqual(review_package["runs"][0]["storyboard"]["title"], "Design parity proof")
        self.assertEqual(review_package["runs"][0]["storyboard"]["steps"][0]["label"], "Launch")
        self.assertEqual(review_package["runs"][0]["context"]["plugin"], "PulpSynth")
        self.assertTrue(review_package["runs"][0]["fallback"]["internal_ephemeral"])
        self.assertEqual(review_package["runs"][0]["fallback"]["serve_urls"], ["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"])
        self.assertIn("desktop serve", review_package["runs"][0]["fallback"]["serve_command"])
        self.assertIn("# Gallery <One>", review_text)
        self.assertIn("looks good to me", review_text)
        self.assertIn("desktop serve` prints candidate URLs", review_text)
        self.assertIn("PULP_DESKTOP_SERVE_HOSTS", review_text)
        self.assertIn("Candidate watch URL: `http://100.64.0.10:8765/`", review_text)
        self.assertIn("proof.issue.mp4", review_text)
        self.assertIn("proof.small.mp4", review_text)
        self.assertIn("proof-composed.mp4", review_text)
        self.assertIn("fits configured attachment budget", review_text)
        self.assertIn("Issue variant: `transcoded` via `balanced-720p`", review_text)
        self.assertIn("Small video size: 8.0 MB (fits 10 MB budget)", review_text)
        self.assertIn("Small variant: `transcoded` via `compact-540p`", review_text)
        self.assertIn("Attachment action: Attach `", review_text)
        self.assertIn("desktop verdict", review_text)
        self.assertIn("--approved --issue-url <issue-url>", review_text)
        self.assertIn("--needs-work --notes", review_text)
        self.assertIn("Proof template: `design-parity`", review_text)
        self.assertIn("Command: `./build/pulp --inspect`", review_text)
        self.assertIn("Source: `mode=exact-sha, branch=feat/validation-video-proof, sha=abc123`", review_text)
        self.assertIn("Host: `macstudio`", review_text)
        self.assertIn("Adapter: `macos-local`", review_text)
        self.assertIn("Manifest: `", review_text)
        self.assertIn("Focus component: `bypass-toggle`", review_text)
        self.assertIn("Action marker: `bypass-toggle`", review_text)
        self.assertIn("Action point: `", review_text)
        self.assertIn("Storyboard:", review_text)
        self.assertIn("Launch: REAPER / PulpSynth / clap", review_text)
        self.assertIn("Focus: bypass-toggle via id: bypass-toggle", review_text)
        self.assertIn("Context plugin: `PulpSynth`", review_text)
        self.assertIn("Context format: `clap`", review_text)
        self.assertIn("Source reference: `", review_text)
        self.assertIn("Proof note: Source import matches the native render.", review_text)

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
            "serve_urls": ["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"],
            "runs": [
                {
                    "target": "mac",
                    "adapter": "macos-local",
                    "action": "click",
                    "label": "component",
                    "host": "macstudio",
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
                        "serve_urls": ["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"],
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
        self.assertEqual(draft["attachment_checks"][0]["path"], str(issue_video))
        self.assertTrue(draft["attachment_checks"][0]["fits_attachment_budget"])
        self.assertEqual(draft["serve_urls"], ["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"])
        self.assertEqual(len(draft["fallback_links"]), 1)
        self.assertEqual(draft["fallback_links"][0]["serve_urls"], ["http://127.0.0.1:8765/", "http://100.64.0.10:8765/"])
        self.assertTrue(draft["fallback_links"][0]["internal_ephemeral"])
        self.assertIn("gh issue create --repo danielraffel/pulp", draft["create_command"])
        self.assertIn("looks good to me", draft["body"])
        self.assertIn("Attach MP4: `", draft["body"])
        self.assertIn("Command: `./build/pulp --inspect`", draft["body"])
        self.assertIn("Source: `mode=exact-sha, branch=feature/video, sha=abc123`", draft["body"])
        self.assertIn("Host: `macstudio`", draft["body"])
        self.assertIn("Adapter: `macos-local`", draft["body"])
        self.assertIn("Manifest: `", draft["body"])
        self.assertIn("Storyboard:", draft["body"])
        self.assertIn("Launch: mac/click", draft["body"])
        self.assertIn("Action: click: bypass-toggle", draft["body"])
        self.assertIn("Candidate watch URL: `http://100.64.0.10:8765/`", draft["body"])
        self.assertIn("Context component: `bypass-toggle`", draft["body"])
        self.assertIn("use the served report link", draft["body"])

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

    def test_manifest_scanning_and_run_rollups_use_latest_passing_proof(self) -> None:
        root = self.artifact_root(self.config)
        old_bundle = root / "mac" / "smoke" / "20260522-old"
        new_bundle = root / "mac" / "smoke" / "20260522-new"
        for bundle in (old_bundle, new_bundle):
            bundle.mkdir(parents=True)
        (old_bundle / "manifest.json").write_text(
            json.dumps(
                {
                    "target": "mac",
                    "action": "smoke",
                    "label": "old",
                    "completed_at": "2026-05-22T12:00:00+00:00",
                    "status": "pass",
                    "source": {"mode": "exact-sha", "sha": "a" * 40},
                }
            )
        )
        (new_bundle / "manifest.json").write_text(
            json.dumps(
                {
                    "target": "mac",
                    "action": "smoke",
                    "label": "new",
                    "completed_at": "2026-05-22T13:00:00+00:00",
                    "status": "error",
                    "source": {"mode": "exact-sha", "sha": "b" * 40},
                }
            )
        )

        manifests = self.mod.desktop_run_manifests(
            self.config,
            target_name="mac",
            action="smoke",
            desktop_artifact_root_fn=self.artifact_root,
        )
        self.assertEqual([manifest["label"] for manifest in manifests], ["new", "old"])
        self.assertEqual(manifests[0]["artifacts"]["bundle_dir"], str(new_bundle))

        def manifests_fn(config: dict, **kwargs):
            return self.mod.desktop_run_manifests(config, desktop_artifact_root_fn=self.artifact_root, **kwargs)

        def summaries_fn(config: dict, **kwargs):
            return self.mod.desktop_proof_summaries(
                config,
                desktop_run_manifests_fn=manifests_fn,
                desktop_run_summary_fn=self.mod.desktop_run_summary,
                **kwargs,
            )

        self.mod.write_desktop_run_rollups(
            self.config,
            target_name="mac",
            desktop_rollup_dir_fn=lambda config, target_name=None: self.mod.desktop_rollup_dir(
                config,
                target_name,
                desktop_artifact_root_fn=self.artifact_root,
            ),
            desktop_run_manifests_fn=manifests_fn,
            desktop_run_summary_fn=self.mod.desktop_run_summary,
            desktop_proof_summaries_fn=summaries_fn,
            atomic_write_text_fn=self.atomic_write,
        )

        latest_run = json.loads((root / "mac" / "latest-run.json").read_text())
        latest_proof = json.loads((root / "mac" / "latest-proof.json").read_text())
        self.assertEqual(latest_run["label"], "new")
        self.assertEqual(latest_run["run_status"], "error")
        self.assertEqual(latest_proof["latest_run"]["label"], "old")

    def test_publish_reports_and_prune_filter_edge_cases(self) -> None:
        old_dir = self.publish_root(self.config) / "old"
        new_dir = self.publish_root(self.config) / "new"
        bad_dir = self.publish_root(self.config) / "bad"
        for path in (old_dir, new_dir, bad_dir):
            path.mkdir(parents=True)
        (old_dir / "index.json").write_text(json.dumps({"generated_at": "2026-01-01T00:00:00Z", "label": "old"}))
        (new_dir / "index.json").write_text(json.dumps({"generated_at": "2026-01-02T00:00:00Z", "label": "new"}))
        (bad_dir / "index.json").write_text("{")

        reports = self.mod.desktop_publish_reports(self.config, desktop_publish_root_fn=self.publish_root)
        self.assertEqual([report["label"] for report in reports], ["new", "old"])

        bundle_a = self.root / "bundle-a"
        bundle_b = self.root / "bundle-b"
        bundle_c = self.root / "bundle-c"
        for bundle in (bundle_a, bundle_b, bundle_c):
            bundle.mkdir()
        manifests = [
            {"completed_at": "2999-01-03T00:00:00+00:00", "artifacts": {"bundle_dir": str(bundle_a)}},
            {"completed_at": "2000-01-01T00:00:00+00:00", "artifacts": {"bundle_dir": str(bundle_b)}},
            {"completed_at": "2000-01-01T00:00:00+00:00", "artifacts": {"bundle_dir": str(bundle_b)}},
            {"completed_at": "invalid", "artifacts": {"bundle_dir": str(bundle_c)}},
        ]
        removed = self.mod.prune_desktop_run_manifests(
            self.config,
            older_than_days=1,
            desktop_run_manifests_fn=lambda _config, **_kwargs: manifests,
        )
        self.assertEqual(removed, [bundle_b])


if __name__ == "__main__":
    unittest.main()
