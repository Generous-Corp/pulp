#!/usr/bin/env python3
"""Tests for the desktop review verdict command."""

from argparse import Namespace
import json
import subprocess
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_review_commands_cli.py")


class FakeProc:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


class DesktopReviewVerdictTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.manifest = self.root / "manifest.json"
        self.manifest.write_text(json.dumps({"label": "demo"}))
        self.written = {}

    def tearDown(self):
        self.tmp.cleanup()

    def _args(self, **over):
        base = dict(
            manifest=str(self.manifest),
            approved=False,
            needs_work=False,
            notes="",
            reviewer="",
            issue_url="",
            comment_issue=False,
            close_issue=False,
            close_reason="completed",
            json=False,
        )
        base.update(over)
        return Namespace(**base)

    def _atomic_write(self, path, text):
        Path(path).write_text(text)

    def _run(self, args, run_fn=None):
        out = []
        rc = self.mod.cmd_desktop_verdict(
            args,
            now_iso_fn=lambda: "2026-06-16T00:00:00+00:00",
            atomic_write_text_fn=self._atomic_write,
            run_fn=run_fn or (lambda *a, **k: FakeProc()),
            print_fn=out.append,
        )
        return rc, "\n".join(out)

    def test_missing_manifest_errors(self):
        rc, out = self._run(self._args(manifest=str(self.root / "nope.json"), approved=True))
        self.assertEqual(rc, 1)
        self.assertIn("manifest not found", out)

    def test_approved_records_review_and_writes_verdict(self):
        rc, out = self._run(self._args(approved=True, notes="looks good"))
        self.assertEqual(rc, 0)
        manifest = json.loads(self.manifest.read_text())
        self.assertEqual(manifest["review"]["status"], "approved")
        self.assertTrue(manifest["review"]["close_review_issue"])
        self.assertTrue((self.root / "review-verdict.md").exists())
        self.assertTrue((self.root / "review-verdict.json").exists())

    def test_needs_work_sets_follow_up(self):
        rc, out = self._run(self._args(needs_work=True, notes="fix the knob"))
        self.assertEqual(rc, 0)
        manifest = json.loads(self.manifest.read_text())
        self.assertEqual(manifest["review"]["status"], "needs-work")
        self.assertFalse(manifest["review"]["close_review_issue"])
        self.assertTrue(manifest["review"]["follow_up_required"])

    def test_comment_issue_requires_url(self):
        rc, out = self._run(self._args(approved=True, comment_issue=True))
        self.assertEqual(rc, 1)
        self.assertIn("--comment-issue requires --issue-url", out)

    def test_comment_issue_invokes_gh(self):
        calls = []

        def run_fn(argv, **kw):
            calls.append(argv)
            return FakeProc(returncode=0)

        rc, out = self._run(
            self._args(approved=True, comment_issue=True, issue_url="https://gh/issue/1"),
            run_fn=run_fn,
        )
        self.assertEqual(rc, 0)
        self.assertEqual(calls[0][:3], ["gh", "issue", "comment"])

    def test_close_issue_requires_approved(self):
        rc, out = self._run(self._args(needs_work=True, close_issue=True, issue_url="https://gh/issue/1"))
        self.assertEqual(rc, 1)
        self.assertIn("--close-issue requires --approved", out)


class DesktopReviewCommandTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.printed: list[str] = []

    def print_line(self, line: str):
        self.printed.append(line)

    def test_review_issue_writes_local_draft_from_report_directory(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            report_dir = Path(tmpdir) / "report"
            report_dir.mkdir()
            run_dir = report_dir / "runs" / "component"
            run_dir.mkdir(parents=True)
            (run_dir / "manifest.json").write_text("{}\n")
            package_path = report_dir / "review-package.json"
            package_path.write_text(
                json.dumps({"label": "Video Proof", "runs": [{"bundle_dir": str(run_dir)}]}) + "\n"
            )
            manifest_map_path = report_dir / "review-manifest-map.json"
            writes = []

            def draft(review_package: dict, **kwargs):
                self.assertEqual(review_package["label"], "Video Proof")
                self.assertEqual(kwargs["package_path"], package_path.resolve())
                self.assertEqual(kwargs["title"], "Review video")
                self.assertEqual(kwargs["repo"], "danielraffel/pulp")
                self.assertTrue(kwargs["check_files"])
                return {
                    "kind": "desktop-video-proof-github-issue-draft",
                    "title": "Review video",
                    "body": "# Review video\n",
                    "body_file": str(report_dir / "github-issue.md"),
                    "json_file": str(report_dir / "github-issue.json"),
                    "attachments": [{"path": str(report_dir / "proof.issue.mp4")}],
                    "fallback_links": [],
                    "create_command": "gh issue create --repo danielraffel/pulp --title Review --body-file github-issue.md",
                }

            result = self.mod.cmd_desktop_review_issue(
                Namespace(
                    path=str(report_dir),
                    title="Review video",
                    repo="danielraffel/pulp",
                    body_output=None,
                    json_output=None,
                    manifest_map_output=str(manifest_map_path),
                    check_files=True,
                    create=True,
                    label=["video-review"],
                    assignee=["@me"],
                    json=False,
                ),
                desktop_review_issue_draft_fn=draft,
                atomic_write_text_fn=lambda path, text: writes.append((path, text)) or path.write_text(text),
                run_fn=lambda argv, **_kwargs: subprocess.CompletedProcess(
                    argv,
                    0,
                    "https://github.com/danielraffel/pulp/issues/123\n",
                    "",
                ),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            self.assertEqual(writes[0][0], report_dir / "github-issue.md")
            self.assertEqual(writes[1][0], manifest_map_path.resolve())
            self.assertEqual(writes[2][0], report_dir / "github-issue.json")
            draft_json = json.loads((report_dir / "github-issue.json").read_text())
            self.assertEqual(draft_json["issue_url"], "https://github.com/danielraffel/pulp/issues/123")
            self.assertEqual(draft_json["manifest_map"]["path"], str(manifest_map_path.resolve()))
            self.assertEqual(draft_json["manifest_map"]["status"], "written")
            self.assertIsNone(draft_json["manifest_map"]["error"])
            self.assertIn("desktop review-issue", draft_json["review_create_command"])
            self.assertIn("--repo danielraffel/pulp", draft_json["review_create_command"])
            self.assertIn("--check-files", draft_json["review_create_command"])
            self.assertIn("--create", draft_json["review_create_command"])
            self.assertIn("--label video-review", draft_json["review_create_command"])
            self.assertIn("--assignee @me", draft_json["review_create_command"])
            self.assertIn(f"--manifest-map-output {manifest_map_path.resolve()}", draft_json["review_create_command"])
            self.assertIn("--title 'Review video'", draft_json["review_create_command"])
            self.assertIn("desktop review-watch", draft_json["review_watch_command"])
            self.assertIn("--label video-review", draft_json["review_watch_command"])
            self.assertIn(f"--manifest-map {manifest_map_path.resolve()}", draft_json["review_watch_command"])
            issue_body = (report_dir / "github-issue.md").read_text()
            self.assertIn("Batch Review Create", issue_body)
            self.assertIn(draft_json["review_create_command"], issue_body)
            self.assertIn("Batch Review Watch", issue_body)
            self.assertIn(draft_json["review_watch_command"], issue_body)
            manifest_map = json.loads(manifest_map_path.read_text())
            self.assertEqual(
                manifest_map,
                {
                    "https://github.com/danielraffel/pulp/issues/123": str(run_dir / "manifest.json"),
                    "123": str(run_dir / "manifest.json"),
                    "#123": str(run_dir / "manifest.json"),
                },
            )
            self.assertIn("--label", draft_json["create_result"]["command"])
            self.assertIn("video-review", draft_json["create_result"]["command"])
            self.assertIn("--assignee", draft_json["create_result"]["command"])
            self.assertIn("@me", draft_json["create_result"]["command"])
            self.assertIn("review issue draft ready", self.printed[0])
            self.assertTrue(any("attachments: 1" in line for line in self.printed))
            self.assertTrue(any("issue_url: https://github.com/danielraffel/pulp/issues/123" in line for line in self.printed))
            self.assertTrue(any(f"manifest_map: {manifest_map_path.resolve()}" in line for line in self.printed))

    def test_review_issue_manifest_map_output_requires_create(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            report_dir = Path(tmpdir) / "report"
            report_dir.mkdir()
            run_dir = report_dir / "runs" / "component"
            run_dir.mkdir(parents=True)
            (run_dir / "manifest.json").write_text("{}\n")
            package_path = report_dir / "review-package.json"
            package_path.write_text(
                json.dumps({"label": "Video Proof", "runs": [{"bundle_dir": str(run_dir)}]}) + "\n"
            )
            manifest_map_path = report_dir / "review-manifest-map.json"

            result = self.mod.cmd_desktop_review_issue(
                Namespace(
                    path=str(report_dir),
                    title=None,
                    repo="danielraffel/pulp",
                    body_output=None,
                    json_output=None,
                    manifest_map_output=str(manifest_map_path),
                    check_files=False,
                    create=False,
                    label=[],
                    assignee=[],
                    json=True,
                ),
                desktop_review_issue_draft_fn=lambda _package, **_kwargs: {
                    "kind": "desktop-video-proof-github-issue-draft",
                    "title": "Review video",
                    "body": "# Review video\n",
                    "body_file": str(report_dir / "github-issue.md"),
                    "json_file": str(report_dir / "github-issue.json"),
                    "attachments": [],
                    "fallback_links": [],
                    "create_command": "gh issue create --repo danielraffel/pulp --title Review --body-file github-issue.md",
                },
                atomic_write_text_fn=lambda path, text: path.write_text(text),
                run_fn=lambda *_args, **_kwargs: self.fail("gh should not run for draft-only review issue"),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            self.assertFalse(manifest_map_path.exists())
            payload = json.loads(self.printed[-1])
            self.assertNotIn("manifest_map_file", payload)
            self.assertEqual(payload["manifest_map"]["path"], str(manifest_map_path.resolve()))
            self.assertEqual(payload["manifest_map"]["entries"], {})
            self.assertIsNone(payload["manifest_map"]["error"])
            self.assertEqual(payload["manifest_map"]["status"], "requires-create")
            self.assertIn("desktop review-issue", payload["review_create_command"])
            self.assertIn("--repo danielraffel/pulp", payload["review_create_command"])
            self.assertIn("--create", payload["review_create_command"])
            self.assertIn(f"--manifest-map-output {manifest_map_path.resolve()}", payload["review_create_command"])
            self.assertIn("desktop review-watch", payload["review_watch_command"])
            self.assertIn("--label video-review", payload["review_watch_command"])
            self.assertIn(f"--manifest-map {manifest_map_path.resolve()}", payload["review_watch_command"])
            issue_body = (report_dir / "github-issue.md").read_text()
            self.assertIn("Batch Review Create", issue_body)
            self.assertIn(payload["review_create_command"], issue_body)
            self.assertIn("Batch Review Watch", issue_body)
            self.assertIn(payload["review_watch_command"], issue_body)

    def test_review_issue_create_failure_writes_failed_draft_json(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            report_dir = Path(tmpdir) / "report"
            report_dir.mkdir()
            package_path = report_dir / "review-package.json"
            package_path.write_text(json.dumps({"label": "Video Proof", "runs": []}) + "\n")

            result = self.mod.cmd_desktop_review_issue(
                Namespace(
                    path=str(report_dir),
                    title=None,
                    repo="danielraffel/pulp",
                    body_output=None,
                    json_output=None,
                    manifest_map_output=None,
                    check_files=False,
                    create=True,
                    label=[],
                    assignee=[],
                    json=True,
                ),
                desktop_review_issue_draft_fn=lambda _package, **_kwargs: {
                    "kind": "desktop-video-proof-github-issue-draft",
                    "title": "Review video",
                    "body": "# Review video\n",
                    "body_file": str(report_dir / "github-issue.md"),
                    "json_file": str(report_dir / "github-issue.json"),
                    "attachments": [],
                    "fallback_links": [],
                    "create_command": "gh issue create --repo danielraffel/pulp --title Review --body-file github-issue.md",
                },
                atomic_write_text_fn=lambda path, text: path.write_text(text),
                run_fn=lambda argv, **_kwargs: subprocess.CompletedProcess(argv, 1, "", "auth required\n"),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 1)
            self.assertEqual(self.printed[-1], "Error: auth required")
            draft_json = json.loads((report_dir / "github-issue.json").read_text())
            self.assertEqual(draft_json["create_result"]["returncode"], 1)

    def test_review_issue_manifest_map_skips_ambiguous_review_packages(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            report_dir = Path(tmpdir) / "report"
            run_a = report_dir / "runs" / "a"
            run_b = report_dir / "runs" / "b"
            run_a.mkdir(parents=True)
            run_b.mkdir(parents=True)
            (run_a / "manifest.json").write_text("{}\n")
            (run_b / "manifest.json").write_text("{}\n")
            package_path = report_dir / "review-package.json"
            package_path.write_text(
                json.dumps(
                    {
                        "label": "Video Proof",
                        "runs": [
                            {"bundle_dir": str(run_a)},
                            {"bundle_dir": str(run_b)},
                        ],
                    }
                )
                + "\n"
            )
            manifest_map_path = report_dir / "review-manifest-map.json"

            result = self.mod.cmd_desktop_review_issue(
                Namespace(
                    path=str(report_dir),
                    title=None,
                    repo="danielraffel/pulp",
                    body_output=None,
                    json_output=None,
                    manifest_map_output=str(manifest_map_path),
                    check_files=False,
                    create=True,
                    label=[],
                    assignee=[],
                    json=True,
                ),
                desktop_review_issue_draft_fn=lambda _package, **_kwargs: {
                    "kind": "desktop-video-proof-github-issue-draft",
                    "title": "Review video",
                    "body": "# Review video\n",
                    "body_file": str(report_dir / "github-issue.md"),
                    "json_file": str(report_dir / "github-issue.json"),
                    "attachments": [],
                    "fallback_links": [],
                    "create_command": "gh issue create --repo danielraffel/pulp --title Review --body-file github-issue.md",
                },
                atomic_write_text_fn=lambda path, text: path.write_text(text),
                run_fn=lambda argv, **_kwargs: subprocess.CompletedProcess(
                    argv,
                    0,
                    "https://github.com/danielraffel/pulp/issues/124\n",
                    "",
                ),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            self.assertFalse(manifest_map_path.exists())
            payload = json.loads(self.printed[-1])
            self.assertEqual(payload["manifest_map"]["entries"], {})
            self.assertEqual(payload["manifest_map"]["error"], "expected exactly one run manifest, found 2")
            self.assertEqual(payload["manifest_map"]["status"], "skipped")
            self.assertIn("desktop review-watch", payload["review_watch_command"])

    def test_review_status_detects_approval_and_suggests_verdict(self):
        issue_payload = {
            "state": "OPEN",
            "url": "https://github.com/danielraffel/pulp/issues/123",
            "comments": [
                {"body": "needs another pass", "author": {"login": "reviewer"}, "url": "https://example/comment/1"},
                {"body": "Looks good to me", "author": {"login": "daniel"}, "url": "https://example/comment/2"},
            ],
        }
        calls = []

        result = self.mod.cmd_desktop_review_status(
            Namespace(
                issue_url="https://github.com/danielraffel/pulp/issues/123",
                repo="danielraffel/pulp",
                manifest="/tmp/run/manifest.json",
                close_issue=True,
                json=True,
            ),
            run_fn=lambda argv, **_kwargs: calls.append(argv) or subprocess.CompletedProcess(argv, 0, json.dumps(issue_payload), ""),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        self.assertEqual(
            calls[0],
            [
                "gh",
                "issue",
                "view",
                "https://github.com/danielraffel/pulp/issues/123",
                "--json",
                "state,url,comments",
                "--repo",
                "danielraffel/pulp",
            ],
        )
        payload = json.loads(self.printed[-1])
        self.assertTrue(payload["approved"])
        self.assertEqual(payload["approval_comment"]["author"]["login"], "daniel")
        self.assertIn("--approved", payload["verdict_command"])
        self.assertIn("/tmp/run/manifest.json", payload["verdict_command"])
        self.assertIn("--issue-url https://github.com/danielraffel/pulp/issues/123", payload["verdict_command"])
        self.assertIn("--close-issue", payload["verdict_command"])

    def test_review_status_reports_pending_and_gh_errors(self):
        pending_payload = {
            "state": "OPEN",
            "url": "https://github.com/danielraffel/pulp/issues/123",
            "comments": [{"body": "not yet", "author": {"login": "reviewer"}}],
        }
        result = self.mod.cmd_desktop_review_status(
            Namespace(
                issue_url="https://github.com/danielraffel/pulp/issues/123",
                repo=None,
                manifest=None,
                close_issue=False,
                json=False,
            ),
            run_fn=lambda argv, **_kwargs: subprocess.CompletedProcess(argv, 0, json.dumps(pending_payload), ""),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertIn("  approved: false", self.printed)
        self.assertIn("  waiting_for: looks good to me", self.printed)

        self.printed.clear()
        result = self.mod.cmd_desktop_review_status(
            Namespace(
                issue_url="https://github.com/danielraffel/pulp/issues/123",
                repo=None,
                manifest=None,
                close_issue=False,
                json=False,
            ),
            run_fn=lambda argv, **_kwargs: subprocess.CompletedProcess(argv, 1, "", "auth required\n"),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 1)
        self.assertEqual(self.printed[-1], "Error: auth required")

        self.printed.clear()
        result = self.mod.cmd_desktop_review_status(
            Namespace(
                issue_url="https://github.com/danielraffel/pulp/issues/123",
                repo=None,
                manifest=None,
                close_issue=False,
                json=False,
            ),
            run_fn=lambda argv, **_kwargs: subprocess.CompletedProcess(argv, 0, "{nope", ""),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 1)
        self.assertIn("invalid gh issue view JSON", self.printed[-1])

    def test_review_status_detects_needs_work_and_suggests_verdict(self):
        issue_payload = {
            "state": "OPEN",
            "url": "https://github.com/danielraffel/pulp/issues/123",
            "comments": [
                {"body": "Looks good to me", "author": {"login": "daniel"}, "url": "https://example/comment/1"},
                {
                    "body": "Needs work: zoom starts too late",
                    "author": {"login": "reviewer"},
                    "url": "https://example/comment/2",
                },
            ],
        }

        result = self.mod.cmd_desktop_review_status(
            Namespace(
                issue_url="https://github.com/danielraffel/pulp/issues/123",
                repo="danielraffel/pulp",
                manifest="/tmp/run/manifest.json",
                close_issue=True,
                json=True,
            ),
            run_fn=lambda argv, **_kwargs: subprocess.CompletedProcess(argv, 0, json.dumps(issue_payload), ""),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        payload = json.loads(self.printed[-1])
        self.assertFalse(payload["approved"])
        self.assertTrue(payload["needs_work"])
        self.assertEqual(payload["needs_work_comment"]["author"]["login"], "reviewer")
        self.assertIn("--needs-work", payload["verdict_command"])
        self.assertIn("--issue-url https://github.com/danielraffel/pulp/issues/123", payload["verdict_command"])
        self.assertIn("--comment-issue", payload["verdict_command"])
        self.assertIn("--notes 'Needs work: zoom starts too late'", payload["verdict_command"])
        self.assertNotIn("--close-issue", payload["verdict_command"])

    def test_review_watch_detects_approved_issues_and_uses_state_cache(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            state_file = Path(tmpdir) / "review-watch.json"
            manifest_map = Path(tmpdir) / "manifest-map.json"
            manifest_map.write_text(json.dumps({"123": "/tmp/run/manifest.json"}) + "\n")
            listed = [
                {
                    "number": 123,
                    "title": "Review validation proof",
                    "url": "https://github.com/danielraffel/pulp/issues/123",
                    "updatedAt": "2026-06-14T12:00:00Z",
                },
                {
                    "number": 124,
                    "title": "Pending validation proof",
                    "url": "https://github.com/danielraffel/pulp/issues/124",
                    "updatedAt": "2026-06-14T12:01:00Z",
                },
            ]
            viewed = {
                "https://github.com/danielraffel/pulp/issues/123": {
                    "state": "OPEN",
                    "number": 123,
                    "title": "Review validation proof",
                    "url": "https://github.com/danielraffel/pulp/issues/123",
                    "updatedAt": "2026-06-14T12:00:00Z",
                    "comments": [
                        {"body": "Looks good to me", "author": {"login": "daniel"}, "url": "https://example/comment/2"}
                    ],
                },
                "https://github.com/danielraffel/pulp/issues/124": {
                    "state": "OPEN",
                    "number": 124,
                    "title": "Pending validation proof",
                    "url": "https://github.com/danielraffel/pulp/issues/124",
                    "updatedAt": "2026-06-14T12:01:00Z",
                    "comments": [{"body": "still watching", "author": {"login": "reviewer"}}],
                },
            }
            calls = []

            def run_fn(argv, **_kwargs):
                calls.append(argv)
                if argv[:3] == ["gh", "issue", "list"]:
                    return subprocess.CompletedProcess(argv, 0, json.dumps(listed), "")
                if argv[:3] == ["gh", "issue", "view"]:
                    return subprocess.CompletedProcess(argv, 0, json.dumps(viewed[argv[3]]), "")
                self.fail(f"unexpected command: {argv}")

            args = Namespace(
                repo="danielraffel/pulp",
                label="video-review",
                state="open",
                state_file=str(state_file),
                manifest_map=str(manifest_map),
                refresh=False,
                close_issue=True,
                interval=0.0,
                max_iterations=1,
                json=True,
            )
            result = self.mod.cmd_desktop_review_watch(args, run_fn=run_fn, print_fn=self.print_line)

            self.assertEqual(result, 0)
            payload = json.loads(self.printed[-1])
            self.assertEqual(payload["kind"], "desktop-video-proof-review-watch")
            self.assertEqual(payload["issue_count"], 2)
            self.assertEqual(payload["checked_count"], 2)
            self.assertEqual(payload["approved_count"], 1)
            approved = next(issue for issue in payload["issues"] if issue["approved"])
            self.assertIn("--approved", approved["verdict_command"])
            self.assertIn("--issue-url https://github.com/danielraffel/pulp/issues/123", approved["verdict_command"])
            self.assertIn("--close-issue", approved["verdict_command"])
            self.assertEqual(sum(1 for call in calls if call[:3] == ["gh", "issue", "view"]), 2)
            state_payload = json.loads(state_file.read_text())
            self.assertTrue(state_payload["issues"]["https://github.com/danielraffel/pulp/issues/123"]["approved"])

            self.printed.clear()
            calls.clear()
            result = self.mod.cmd_desktop_review_watch(args, run_fn=run_fn, print_fn=self.print_line)

            self.assertEqual(result, 0)
            payload = json.loads(self.printed[-1])
            self.assertEqual(payload["checked_count"], 0)
            self.assertEqual(payload["skipped_unchanged_count"], 2)
            self.assertEqual(payload["approved_count"], 1)
            self.assertEqual(sum(1 for call in calls if call[:3] == ["gh", "issue", "view"]), 0)

    def test_review_watch_detects_needs_work_issues(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            manifest_map = Path(tmpdir) / "manifest-map.json"
            manifest_map.write_text(json.dumps({"125": "/tmp/needs-work/manifest.json"}) + "\n")
            listed = [
                {
                    "number": 125,
                    "title": "Review validation proof",
                    "url": "https://github.com/danielraffel/pulp/issues/125",
                    "updatedAt": "2026-06-14T12:02:00Z",
                }
            ]
            viewed = {
                "state": "OPEN",
                "number": 125,
                "title": "Review validation proof",
                "url": "https://github.com/danielraffel/pulp/issues/125",
                "updatedAt": "2026-06-14T12:02:00Z",
                "comments": [
                    {
                        "body": "Needs changes: recapture after centering the component",
                        "author": {"login": "reviewer"},
                        "url": "https://example/comment/3",
                    }
                ],
            }

            def run_fn(argv, **_kwargs):
                if argv[:3] == ["gh", "issue", "list"]:
                    return subprocess.CompletedProcess(argv, 0, json.dumps(listed), "")
                if argv[:3] == ["gh", "issue", "view"]:
                    return subprocess.CompletedProcess(argv, 0, json.dumps(viewed), "")
                self.fail(f"unexpected command: {argv}")

            result = self.mod.cmd_desktop_review_watch(
                Namespace(
                    repo="danielraffel/pulp",
                    label="video-review",
                    state="open",
                    state_file=None,
                    manifest_map=str(manifest_map),
                    refresh=False,
                    close_issue=True,
                    interval=0.0,
                    max_iterations=1,
                    json=True,
                ),
                run_fn=run_fn,
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            payload = json.loads(self.printed[-1])
            self.assertEqual(payload["approved_count"], 0)
            self.assertEqual(payload["needs_work_count"], 1)
            issue = payload["issues"][0]
            self.assertTrue(issue["needs_work"])
            self.assertIn("--needs-work", issue["verdict_command"])
            self.assertIn("/tmp/needs-work/manifest.json", issue["verdict_command"])
            self.assertIn("--comment-issue", issue["verdict_command"])
            self.assertIn("--notes 'Needs changes: recapture after centering the component'", issue["verdict_command"])
            self.assertNotIn("--close-issue", issue["verdict_command"])

    def test_review_watch_recomputes_cached_verdict_when_manifest_map_arrives(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            state_file = Path(tmpdir) / "review-watch.json"
            manifest_map = Path(tmpdir) / "manifest-map.json"
            manifest_map.write_text(json.dumps({"126": "/tmp/run/manifest.json"}) + "\n")
            listed = [
                {
                    "number": 126,
                    "title": "Review validation proof",
                    "url": "https://github.com/danielraffel/pulp/issues/126",
                    "updatedAt": "2026-06-14T12:03:00Z",
                }
            ]
            viewed = {
                "state": "OPEN",
                "number": 126,
                "title": "Review validation proof",
                "url": "https://github.com/danielraffel/pulp/issues/126",
                "updatedAt": "2026-06-14T12:03:00Z",
                "comments": [
                    {"body": "Looks good to me", "author": {"login": "daniel"}, "url": "https://example/comment/4"}
                ],
            }
            calls = []

            def run_fn(argv, **_kwargs):
                calls.append(argv)
                if argv[:3] == ["gh", "issue", "list"]:
                    return subprocess.CompletedProcess(argv, 0, json.dumps(listed), "")
                if argv[:3] == ["gh", "issue", "view"]:
                    return subprocess.CompletedProcess(argv, 0, json.dumps(viewed), "")
                self.fail(f"unexpected command: {argv}")

            first_args = Namespace(
                repo="danielraffel/pulp",
                label="video-review",
                state="open",
                state_file=str(state_file),
                manifest_map=None,
                refresh=False,
                close_issue=False,
                interval=0.0,
                max_iterations=1,
                json=True,
            )
            result = self.mod.cmd_desktop_review_watch(first_args, run_fn=run_fn, print_fn=self.print_line)

            self.assertEqual(result, 0)
            first_payload = json.loads(self.printed[-1])
            self.assertTrue(first_payload["issues"][0]["approved"])
            self.assertIsNone(first_payload["issues"][0]["verdict_command"])
            self.assertEqual(sum(1 for call in calls if call[:3] == ["gh", "issue", "view"]), 1)

            self.printed.clear()
            calls.clear()
            second_args = Namespace(
                repo="danielraffel/pulp",
                label="video-review",
                state="open",
                state_file=str(state_file),
                manifest_map=str(manifest_map),
                refresh=False,
                close_issue=True,
                interval=0.0,
                max_iterations=1,
                json=True,
            )
            result = self.mod.cmd_desktop_review_watch(second_args, run_fn=run_fn, print_fn=self.print_line)

            self.assertEqual(result, 0)
            second_payload = json.loads(self.printed[-1])
            self.assertEqual(second_payload["checked_count"], 0)
            self.assertEqual(second_payload["skipped_unchanged_count"], 1)
            issue = second_payload["issues"][0]
            self.assertEqual(issue["manifest"], "/tmp/run/manifest.json")
            self.assertIn("--approved", issue["verdict_command"])
            self.assertIn("/tmp/run/manifest.json", issue["verdict_command"])
            self.assertIn("--issue-url https://github.com/danielraffel/pulp/issues/126", issue["verdict_command"])
            self.assertIn("--close-issue", issue["verdict_command"])
            self.assertEqual(sum(1 for call in calls if call[:3] == ["gh", "issue", "view"]), 0)

    def test_review_watch_reports_gh_and_manifest_map_errors(self):
        result = self.mod.cmd_desktop_review_watch(
            Namespace(
                repo=None,
                label="video-review",
                state="open",
                state_file=None,
                manifest_map="/tmp/does-not-exist.json",
                refresh=False,
                close_issue=False,
                interval=0.0,
                max_iterations=1,
                json=False,
            ),
            run_fn=lambda *_args, **_kwargs: self.fail("gh should not run"),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 1)
        self.assertIn("could not read manifest map", self.printed[-1])

        self.printed.clear()
        result = self.mod.cmd_desktop_review_watch(
            Namespace(
                repo=None,
                label="video-review",
                state="open",
                state_file=None,
                manifest_map=None,
                refresh=False,
                close_issue=False,
                interval=0.0,
                max_iterations=1,
                json=False,
            ),
            run_fn=lambda argv, **_kwargs: subprocess.CompletedProcess(argv, 1, "", "auth required\n"),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 1)
        self.assertEqual(self.printed[-1], "Error: auth required")

    def test_review_issue_reports_missing_package(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            result = self.mod.cmd_desktop_review_issue(
                Namespace(
                    path=str(Path(tmpdir) / "missing"),
                    title=None,
                    repo=None,
                    body_output=None,
                    json_output=None,
                    manifest_map_output=None,
                    check_files=False,
                    json=True,
                ),
                desktop_review_issue_draft_fn=lambda *_args, **_kwargs: self.fail("draft should not run"),
                atomic_write_text_fn=lambda path, text: path.write_text(text),
                print_fn=self.print_line,
            )

        self.assertEqual(result, 1)
        self.assertIn("review package not found", self.printed[-1])

    def test_review_issue_reports_file_check_errors(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            report_dir = Path(tmpdir) / "report"
            report_dir.mkdir()
            package_path = report_dir / "review-package.json"
            package_path.write_text(json.dumps({"label": "Video Proof", "runs": []}) + "\n")

            def fail_file_check(*_args, **_kwargs):
                raise ValueError("run 1 attachment missing: proof.issue.mp4")

            result = self.mod.cmd_desktop_review_issue(
                Namespace(
                    path=str(report_dir),
                    title=None,
                    repo=None,
                    body_output=None,
                    json_output=None,
                    manifest_map_output=None,
                    check_files=True,
                    json=True,
                ),
                desktop_review_issue_draft_fn=fail_file_check,
                atomic_write_text_fn=lambda path, text: self.fail("draft should not be written"),
                print_fn=self.print_line,
            )

        self.assertEqual(result, 1)
        self.assertEqual(self.printed[-1], "Error: run 1 attachment missing: proof.issue.mp4")


if __name__ == "__main__":
    unittest.main()
