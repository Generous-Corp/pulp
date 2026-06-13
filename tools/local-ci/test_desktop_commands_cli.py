#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
from argparse import Namespace
from pathlib import Path
import subprocess
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().with_name("desktop_commands_cli.py")


def load_desktop_commands_cli_module():
    spec = importlib.util.spec_from_file_location("desktop_commands_cli_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class DesktopCommandsCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_desktop_commands_cli_module()
        self.printed: list[str] = []
        self.saved_configs: list[dict] = []

    def print_line(self, line: str):
        self.printed.append(line)

    def desktop_config(self):
        return {
            "desktop_automation": {
                "artifact_root": "runs",
                "publish_mode": "local",
                "publish_branch": "desktop-artifacts",
                "retention_days": 7,
                "targets": {
                    "mac": {
                        "enabled": True,
                        "adapter": "macos-local",
                        "bootstrap": "launchagent",
                        "target_type": "local",
                        "capability_tier": "full",
                        "optional": {"webview_driver": True},
                    }
                },
            }
        }

    def test_desktop_status_builds_text_and_json_payloads(self):
        latest_run = {
            "label": "run",
            "completed_at": "now",
            "interaction_mode": "pulp-app",
            "run_status": "pass",
            "source": {"mode": "legacy", "branch": None, "sha": "a" * 40},
            "proof_scope": "legacy",
            "host": None,
            "artifacts": {
                "screenshot": "after.png",
                "before_screenshot": None,
                "diff_screenshot": None,
                "image_change": None,
                "ui_snapshot": None,
                "bundle_dir": "bundle",
            },
        }
        status_lines_calls = []

        def status_lines(desktop_cfg, target_payloads, **kwargs):
            status_lines_calls.append((desktop_cfg, target_payloads, kwargs))
            return ["Desktop automation:", f"  target={target_payloads[0]['name']}"]

        result = self.mod.cmd_desktop_status(
            Namespace(target=None, json=False),
            load_config_fn=self.desktop_config,
            desktop_receipt_for_fn=lambda _name: {"installed_at": "then"},
            desktop_capabilities_for_fn=lambda *_args: ["screenshot"],
            desktop_optional_capabilities_fn=lambda _optional: ["webview_dom"],
            desktop_run_manifests_fn=lambda *_args, **_kwargs: [{"label": "manifest"}],
            desktop_run_summary_fn=lambda _config, _manifest: latest_run,
            desktop_proof_summaries_fn=lambda *_args, **_kwargs: [{"latest_run": latest_run}],
            normalize_desktop_optional_config_fn=lambda optional: dict(optional or {}),
            desktop_target_contract_fn=lambda name, _target: {"name": name},
            desktop_publish_reports_fn=lambda *_args, **_kwargs: [{"label": "publish"}],
            desktop_status_lines_fn=status_lines,
            short_sha_fn=lambda value: value[:12],
            windows_tooling_detail_fn=lambda *_args, **_kwargs: "",
            windows_repo_checkout_detail_fn=lambda *_args, **_kwargs: "",
            print_fn=self.print_line,
        )

        self.assertEqual(result, 0)
        self.assertEqual(self.printed, ["Desktop automation:", "  target=mac"])
        self.assertEqual(status_lines_calls[0][1][0]["latest_run"]["label"], "run")

        self.printed.clear()
        result = self.mod.cmd_desktop_status(
            Namespace(target="mac", json=True),
            load_config_fn=self.desktop_config,
            desktop_receipt_for_fn=lambda _name: {},
            desktop_capabilities_for_fn=lambda *_args: [],
            desktop_optional_capabilities_fn=lambda _optional: [],
            desktop_run_manifests_fn=lambda *_args, **_kwargs: [],
            desktop_run_summary_fn=lambda _config, _manifest: {},
            desktop_proof_summaries_fn=lambda *_args, **_kwargs: [],
            normalize_desktop_optional_config_fn=lambda optional: dict(optional or {}),
            desktop_target_contract_fn=lambda name, _target: {"name": name},
            desktop_publish_reports_fn=lambda *_args, **_kwargs: [],
            desktop_status_lines_fn=status_lines,
            short_sha_fn=lambda value: value[:12],
            windows_tooling_detail_fn=lambda *_args, **_kwargs: "",
            windows_repo_checkout_detail_fn=lambda *_args, **_kwargs: "",
            print_fn=self.print_line,
        )
        payload = json.loads(self.printed[0])
        self.assertEqual(result, 0)
        self.assertEqual(payload["targets"][0]["name"], "mac")

    def test_desktop_status_reports_missing_config_and_unknown_target(self):
        common = {
            "desktop_receipt_for_fn": lambda _name: {},
            "desktop_capabilities_for_fn": lambda *_args: [],
            "desktop_optional_capabilities_fn": lambda _optional: [],
            "desktop_run_manifests_fn": lambda *_args, **_kwargs: [],
            "desktop_run_summary_fn": lambda _config, _manifest: {},
            "desktop_proof_summaries_fn": lambda *_args, **_kwargs: [],
            "normalize_desktop_optional_config_fn": lambda optional: dict(optional or {}),
            "desktop_target_contract_fn": lambda name, _target: {"name": name},
            "desktop_publish_reports_fn": lambda *_args, **_kwargs: [],
            "desktop_status_lines_fn": lambda *_args, **_kwargs: [],
            "short_sha_fn": lambda value: value[:12],
            "windows_tooling_detail_fn": lambda *_args, **_kwargs: "",
            "windows_repo_checkout_detail_fn": lambda *_args, **_kwargs: "",
            "print_fn": self.print_line,
        }
        result = self.mod.cmd_desktop_status(
            Namespace(target=None, json=False),
            load_config_fn=lambda: (_ for _ in ()).throw(FileNotFoundError("missing config")),
            **common,
        )
        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["Error: missing config"])

        self.printed.clear()
        result = self.mod.cmd_desktop_status(
            Namespace(target="windows", json=False),
            load_config_fn=self.desktop_config,
            **common,
        )
        self.assertEqual(result, 1)
        self.assertIn("unknown desktop target", self.printed[0])

    def test_desktop_video_matrix_outputs_text_json_and_markdown(self):
        result = self.mod.cmd_desktop_video_matrix(
            Namespace(target="mac", scenario="component-zoom", json=False, markdown=False),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        text = "\n".join(self.printed)
        self.assertIn("Desktop validation video proof demo matrix:", text)
        self.assertIn("component-zoom [ready]", text)
        self.assertIn("--recipe component-zoom", text)
        self.assertNotIn("ios-simulator", text)

        self.printed.clear()
        result = self.mod.cmd_desktop_video_matrix(
            Namespace(target=None, scenario=None, json=True, markdown=False),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        payload = json.loads(self.printed[0])
        self.assertEqual(payload["kind"], "desktop-video-proof-demo-matrix")
        self.assertEqual(payload["scenario_count"], 7)
        self.assertIn("reaper-plugin-editor", {item["id"] for item in payload["scenarios"]})

        self.printed.clear()
        result = self.mod.cmd_desktop_video_matrix(
            Namespace(target="ios-simulator", scenario=None, json=False, markdown=True),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        markdown = self.printed[0]
        self.assertIn("# Desktop Validation Video Proof Demo Matrix", markdown)
        self.assertIn("iOS Simulator interaction", markdown)
        self.assertIn("python3 tools/local-ci/local_ci.py simulator video", markdown)
        self.assertIn("python3 tools/local-ci/local_ci.py simulator video-doctor", markdown)
        self.assertNotIn("Standalone app interaction", markdown)

    def test_desktop_config_show_set_and_dispatch(self):
        result = self.mod.cmd_desktop_config_show(
            Namespace(json=False),
            load_config_fn=self.desktop_config,
            desktop_config_show_lines_fn=lambda _cfg: ["Desktop automation config:"],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.printed, ["Desktop automation config:"])

        config = self.desktop_config()
        result = self.mod.cmd_desktop_config_set(
            Namespace(key="target.mac.webview_driver", value="false", json=False),
            load_config_fn=lambda: config,
            save_config_fn=self.saved_configs.append,
            config_path_fn=lambda: Path("/tmp/config.json"),
            normalize_publish_mode_fn=lambda value: value,
            parse_config_bool_fn=lambda value: value.lower() == "true",
            normalize_desktop_config_fn=lambda payload: payload,
            desktop_config_update_lines_fn=lambda payload: [f"{payload['key']} = {payload['value']}"],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertFalse(self.saved_configs[0]["desktop_automation"]["targets"]["mac"]["optional"]["webview_driver"])
        self.assertEqual(self.printed[-1], "target.mac.webview_driver = False")

        calls = []
        result = self.mod.cmd_desktop_config(
            Namespace(desktop_config_command="show"),
            commands={"show": lambda args: calls.append(args) or 7},
            print_fn=self.print_line,
        )
        self.assertEqual(result, 7)
        self.assertEqual(len(calls), 1)

    def test_desktop_config_set_reports_validation_errors(self):
        result = self.mod.cmd_desktop_config_set(
            Namespace(key="retention_days", value="-1", json=False),
            load_config_fn=lambda: {"desktop_automation": {}},
            save_config_fn=self.saved_configs.append,
            config_path_fn=lambda: Path("/tmp/config.json"),
            normalize_publish_mode_fn=lambda value: value,
            parse_config_bool_fn=lambda value: value == "true",
            normalize_desktop_config_fn=lambda payload: payload,
            desktop_config_update_lines_fn=lambda payload: [str(payload)],
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertEqual(self.printed, ["Error: retention_days must be >= 0"])
        self.assertEqual(self.saved_configs, [])

    def test_recent_proof_publish_and_cleanup_paths(self):
        config = self.desktop_config()
        run_manifest = {"label": "run", "target": "mac"}
        result = self.mod.cmd_desktop_recent(
            Namespace(target="mac", action="smoke", limit=1, json=True),
            load_config_fn=lambda: config,
            desktop_run_manifests_fn=lambda *_args, **_kwargs: [run_manifest],
            desktop_run_summary_fn=lambda _config, manifest: {"label": manifest["label"]},
            desktop_recent_lines_fn=lambda summaries, **_kwargs: [f"recent {summaries[0]['label']}"],
            short_sha_fn=lambda value: value[:12],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(self.printed[-1])["runs"], [run_manifest])

        result = self.mod.cmd_desktop_proof(
            Namespace(target="mac", action="smoke", source_mode="legacy", sha=None, branch=None, limit=5, json=False),
            load_config_fn=lambda: config,
            desktop_proof_summaries_fn=lambda *_args, **_kwargs: [],
            desktop_proof_empty_line_fn=lambda **_kwargs: "No desktop proofs found.",
            desktop_proof_lines_fn=lambda *_args, **_kwargs: ["unused"],
            short_sha_fn=lambda value: value[:12],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.printed[-1], "No desktop proofs found.")

        result = self.mod.cmd_desktop_publish(
            Namespace(target="mac", action="smoke", limit=1, output=None, label="gallery", json=False),
            load_config_fn=lambda: config,
            desktop_run_manifests_fn=lambda *_args, **_kwargs: [run_manifest],
            stage_desktop_publish_report_fn=lambda *_args, **kwargs: {"run_count": 1, "serve_urls": kwargs["serve_urls"]},
            desktop_publish_lines_fn=lambda report: [f"published {report['run_count']}"],
            desktop_serve_candidate_urls_fn=lambda host, port: [f"http://127.0.0.1:{port}/", f"http://100.64.0.10:{port}/"],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.printed[-1], "published 1")

        with tempfile.TemporaryDirectory() as tmpdir:
            manifest_path = Path(tmpdir) / "manifest.json"
            manifest_path.write_text(json.dumps({"label": "explicit", "artifacts": {}}) + "\n")
            captured = []
            result = self.mod.cmd_desktop_publish(
                Namespace(target=None, action=None, limit=5, output=None, label="explicit-gallery", manifest=[str(manifest_path)], json=True),
                load_config_fn=lambda: config,
                desktop_run_manifests_fn=lambda *_args, **_kwargs: self.fail("explicit manifest should skip discovery"),
                stage_desktop_publish_report_fn=lambda _config, manifests, **kwargs: captured.append((manifests, kwargs)) or {"run_count": len(manifests)},
                desktop_publish_lines_fn=lambda report: [f"published {report['run_count']}"],
                desktop_serve_candidate_urls_fn=lambda host, port: [f"http://127.0.0.1:{port}/"],
                print_fn=self.print_line,
            )
            self.assertEqual(result, 0)
            self.assertEqual(captured[0][0][0]["label"], "explicit")
            self.assertEqual(captured[0][0][0]["artifacts"]["bundle_dir"], str(Path(tmpdir)))
            self.assertEqual(captured[0][1]["label"], "explicit-gallery")
            self.assertEqual(captured[0][1]["serve_urls"], ["http://127.0.0.1:8765/"])

        served = []
        with tempfile.TemporaryDirectory() as tmpdir:
            artifact_root = Path(tmpdir) / "runs"
            report_dir = artifact_root / "_published" / "report"
            report_dir.mkdir(parents=True)
            (report_dir / "index.html").write_text("<html></html>")
            serve_config = self.desktop_config()
            serve_config["desktop_automation"]["artifact_root"] = str(artifact_root)
            result = self.mod.cmd_desktop_serve(
                Namespace(path=None, host="0.0.0.0", port=8765),
                load_config_fn=lambda: serve_config,
                desktop_publish_reports_fn=lambda *_args, **_kwargs: [{"output_dir": str(report_dir)}],
                desktop_serve_candidate_urls_fn=lambda host, port: [
                    f"http://127.0.0.1:{port}/",
                    f"http://100.64.0.10:{port}/",
                ],
                serve_directory_fn=lambda path, **kwargs: served.append((path, kwargs)),
                print_fn=self.print_line,
            )

        self.assertEqual(result, 0)
        self.assertEqual(served[0][0], report_dir)
        self.assertEqual(served[0][1], {"host": "0.0.0.0", "port": 8765})
        self.assertIn("http://127.0.0.1:8765/", self.printed[-3])
        self.assertIn("http://100.64.0.10:8765/", self.printed[-2])

        self.printed.clear()
        served.clear()
        with tempfile.TemporaryDirectory() as tmpdir:
            artifact_root = Path(tmpdir) / "runs"
            artifact_root.mkdir()
            outside_dir = Path(tmpdir) / "outside"
            outside_dir.mkdir()
            (outside_dir / "index.html").write_text("<html></html>")
            serve_config = self.desktop_config()
            serve_config["desktop_automation"]["artifact_root"] = str(artifact_root)
            result = self.mod.cmd_desktop_serve(
                Namespace(path=str(outside_dir), host="127.0.0.1", port=8765),
                load_config_fn=lambda: serve_config,
                desktop_publish_reports_fn=lambda *_args, **_kwargs: [],
                serve_directory_fn=lambda path, **kwargs: served.append((path, kwargs)),
                print_fn=self.print_line,
            )
        self.assertEqual(result, 1)
        self.assertEqual(served, [])
        self.assertIn("only serves reports under configured publish root", self.printed[-1])

    def test_desktop_serve_candidate_urls_include_tailscale_and_configured_hosts(self):
        calls = []

        def run(cmd, **kwargs):
            calls.append((cmd, kwargs))
            return subprocess.CompletedProcess(cmd, 0, stdout="100.64.0.20\n100.64.0.21\n", stderr="")

        urls = self.mod.desktop_serve_candidate_urls(
            "0.0.0.0",
            8765,
            run_fn=run,
            which_fn=lambda name: "/usr/bin/tailscale" if name == "tailscale" else None,
            environ={"PULP_DESKTOP_SERVE_HOSTS": "blackbook.tailnet.ts.net, 100.64.0.20"},
            hostname_fn=lambda: "macstudio",
        )

        self.assertEqual(
            urls,
            [
                "http://127.0.0.1:8765/",
                "http://macstudio:8765/",
                "http://macstudio.local:8765/",
                "http://blackbook.tailnet.ts.net:8765/",
                "http://100.64.0.20:8765/",
                "http://100.64.0.21:8765/",
            ],
        )
        self.assertEqual(calls[0][0], ["tailscale", "ip", "-4"])

    def test_desktop_serve_candidate_urls_for_loopback_bind_avoid_public_hosts(self):
        urls = self.mod.desktop_serve_candidate_urls(
            "127.0.0.1",
            8765,
            run_fn=lambda *_args, **_kwargs: self.fail("tailscale should not be queried for loopback bind"),
            which_fn=lambda _name: "/usr/bin/tailscale",
            environ={"PULP_DESKTOP_SERVE_HOSTS": "blackbook.tailnet.ts.net"},
            hostname_fn=lambda: "macstudio",
        )

        self.assertEqual(urls, ["http://127.0.0.1:8765/"])

    def test_compose_video_updates_manifest_artifacts(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            run_dir = Path(tmpdir) / "run"
            video_dir = run_dir / "video"
            video_dir.mkdir(parents=True)
            raw_video = video_dir / "proof.mp4"
            raw_video.write_bytes(b"raw")
            manifest_path = run_dir / "manifest.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "label": "video-proof",
                        "artifacts": {
                            "video": str(raw_video),
                        },
                        "video_proof_composition": {
                            "template": "component-zoom",
                            "focus": {"label": "bypass-toggle", "selector": {"click_view_id": "bypass-toggle"}},
                        },
                    }
                )
                + "\n"
            )
            (run_dir / "reference.png").write_bytes(b"png")
            writes = []

            def compose(manifest_path_arg: Path, output_path: Path, **kwargs):
                self.assertEqual(kwargs["template"], "design-parity")
                self.assertEqual(kwargs["source_image"], (run_dir / "reference.png").resolve())
                self.assertEqual(kwargs["source_label"], "Figma reference")
                self.assertEqual(kwargs["title"], "Design parity")
                self.assertEqual(kwargs["notes"], ["Reference matches implementation"])
                output_path.write_bytes(b"composed")
                return {"output": str(output_path), "composer": "remotion"}

            issue_calls = []

            def issue(source_path: Path, output_path: Path, metadata_path: Path, *, attachment_budget_bytes: int):
                self.assertEqual(source_path, (video_dir / "proof-composed.mp4").resolve())
                issue_calls.append((output_path, metadata_path, attachment_budget_bytes))
                output_path.write_bytes(b"issue")
                payload = {"status": "copied", "output": str(output_path), "budget": attachment_budget_bytes}
                metadata_path.write_text(json.dumps(payload) + "\n")
                return payload

            result = self.mod.cmd_desktop_compose_video(
                Namespace(
                    manifest=str(manifest_path),
                    output=None,
                    metadata=None,
                    issue_output=None,
                    issue_metadata=None,
                    small_video=True,
                    small_output=None,
                    small_metadata=None,
                    small_video_budget_mb=10.0,
                    template="design-parity",
                    source_image=str(run_dir / "reference.png"),
                    source_label="Figma reference",
                    title="Design parity",
                    note=["Reference matches implementation"],
                    video_attachment_budget_mb=40.0,
                    json=True,
                ),
                compose_desktop_video_proof_fn=compose,
                create_issue_video_variant_fn=issue,
                atomic_write_text_fn=lambda path, text: writes.append((path, text)) or path.write_text(text),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            payload = json.loads(self.printed[-1])
            self.assertEqual(payload["video_issue"]["status"], "copied")
            self.assertEqual(payload["video_issue"]["budget"], 40_000_000)
            self.assertEqual(payload["video_small"]["budget"], 10_000_000)
            self.assertEqual([call[2] for call in issue_calls], [40_000_000, 10_000_000])
            updated = json.loads(manifest_path.read_text())
            self.assertEqual(updated["video_composed"]["composer"], "remotion")
            self.assertEqual(updated["video_small"]["budget"], 10_000_000)
            self.assertEqual(updated["video_proof_composition"]["template"], "design-parity")
            self.assertTrue(updated["video_proof_composition"]["source_image"].endswith("/reference.png"))
            self.assertEqual(updated["video_proof_composition"]["source_label"], "Figma reference")
            self.assertEqual(updated["video_proof_composition"]["title"], "Design parity")
            self.assertEqual(updated["video_proof_composition"]["notes"], ["Reference matches implementation"])
            self.assertEqual(updated["video_proof_composition"]["focus"]["label"], "bypass-toggle")
            self.assertEqual(updated["video_proof_notes"], ["Reference matches implementation"])
            self.assertTrue(updated["artifacts"]["video_composed"].endswith("/video/proof-composed.mp4"))
            self.assertTrue(updated["artifacts"]["video_composed_metadata"].endswith("/video/composed-metadata.json"))
            self.assertTrue(updated["artifacts"]["video_issue"].endswith("/video/proof.issue.mp4"))
            self.assertTrue(updated["artifacts"]["video_issue_metadata"].endswith("/video/issue-metadata.json"))
            self.assertTrue(updated["artifacts"]["video_small"].endswith("/video/proof.small.mp4"))
            self.assertTrue(updated["artifacts"]["video_small_metadata"].endswith("/video/small-metadata.json"))
            self.assertIn(manifest_path.resolve(), [path for path, _text in writes])

    def test_compose_video_accepts_mobile_simulator_template(self):
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            video_dir = run_dir / "video"
            video_dir.mkdir()
            raw_video = video_dir / "proof.mp4"
            raw_video.write_bytes(b"raw")
            manifest_path = run_dir / "manifest.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "target": "ios-simulator",
                        "action": "video",
                        "label": "ios-simulator-openurl",
                        "interaction": {"mode": "open-url", "label": "open example.com"},
                        "artifacts": {"video": str(raw_video)},
                        "video_proof_composition": {
                            "template": "mobile-simulator",
                            "action_marker": {"kind": "open-url", "label": "open example.com"},
                        },
                    }
                )
                + "\n"
            )

            def compose(_manifest_path: Path, output_path: Path, **kwargs):
                self.assertEqual(kwargs["template"], "mobile-simulator")
                self.assertEqual(kwargs["title"], "Simulator proof")
                self.assertEqual(kwargs["notes"], ["URL opened during capture"])
                output_path.write_bytes(b"composed")
                return {"output": str(output_path), "composer": "remotion"}

            def issue(_source_path: Path, output_path: Path, metadata_path: Path, *, attachment_budget_bytes: int):
                output_path.write_bytes(b"issue")
                payload = {"status": "copied", "output": str(output_path), "budget": attachment_budget_bytes}
                metadata_path.write_text(json.dumps(payload) + "\n")
                return payload

            result = self.mod.cmd_desktop_compose_video(
                Namespace(
                    manifest=str(manifest_path),
                    output=None,
                    metadata=None,
                    issue_output=None,
                    issue_metadata=None,
                    small_video=True,
                    small_output=None,
                    small_metadata=None,
                    small_video_budget_mb=10.0,
                    template="mobile-simulator",
                    source_image=None,
                    source_label=None,
                    title="Simulator proof",
                    note=["URL opened during capture"],
                    video_attachment_budget_mb=100.0,
                    json=True,
                ),
                compose_desktop_video_proof_fn=compose,
                create_issue_video_variant_fn=issue,
                atomic_write_text_fn=lambda path, text: path.write_text(text),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            updated = json.loads(manifest_path.read_text())
            self.assertEqual(updated["video_proof_composition"]["template"], "mobile-simulator")
            self.assertEqual(updated["video_proof_composition"]["action_marker"]["label"], "open example.com")
            self.assertEqual(updated["video_proof_composition"]["title"], "Simulator proof")
            self.assertEqual(updated["video_proof_composition"]["notes"], ["URL opened during capture"])
            self.assertTrue(updated["artifacts"]["video_composed"].endswith("/video/proof-composed.mp4"))
            self.assertTrue(updated["artifacts"]["video_issue"].endswith("/video/proof.issue.mp4"))
            self.assertTrue(updated["artifacts"]["video_small"].endswith("/video/proof.small.mp4"))

    def test_compose_video_reports_missing_manifest(self):
        result = self.mod.cmd_desktop_compose_video(
            Namespace(
                manifest="/tmp/does-not-exist/manifest.json",
                output=None,
                metadata=None,
                issue_output=None,
                issue_metadata=None,
                template=None,
                source_image=None,
                source_label=None,
                title=None,
                video_attachment_budget_mb=100.0,
                json=False,
            ),
            compose_desktop_video_proof_fn=lambda *_args, **_kwargs: {},
            create_issue_video_variant_fn=lambda *_args, **_kwargs: {},
            atomic_write_text_fn=lambda path, text: path.write_text(text),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertIn("manifest not found", self.printed[-1])

    def test_compose_video_rejects_missing_source_image(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            manifest_path = Path(tmpdir) / "manifest.json"
            manifest_path.write_text('{"label":"video-proof","artifacts":{}}\n')
            result = self.mod.cmd_desktop_compose_video(
                Namespace(
                    manifest=str(manifest_path),
                    output=None,
                    metadata=None,
                    issue_output=None,
                    issue_metadata=None,
                    template="design-parity",
                    source_image=str(Path(tmpdir) / "missing.png"),
                    source_label=None,
                    title=None,
                    video_attachment_budget_mb=40.0,
                    json=False,
                ),
                compose_desktop_video_proof_fn=lambda *_args, **_kwargs: self.fail("compose should not run"),
                create_issue_video_variant_fn=lambda *_args, **_kwargs: self.fail("issue variant should not run"),
                atomic_write_text_fn=lambda path, text: path.write_text(text),
                print_fn=self.print_line,
            )

        self.assertEqual(result, 1)
        self.assertIn("source image not found", self.printed[-1])

    def test_verdict_updates_run_manifest(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            manifest_path = Path(tmpdir) / "manifest.json"
            manifest_path.write_text(json.dumps({"label": "video-proof", "artifacts": {}}) + "\n")
            writes = []

            result = self.mod.cmd_desktop_verdict(
                Namespace(
                    manifest=str(manifest_path),
                    approved=True,
                    needs_work=False,
                    notes="looks good",
                    reviewer="daniel",
                    issue_url="https://github.com/example/repo/issues/1",
                    json=True,
                ),
                now_iso_fn=lambda: "2026-06-12T12:00:00+00:00",
                atomic_write_text_fn=lambda path, text: writes.append((path, text)) or path.write_text(text),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            payload = json.loads(self.printed[-1])
            self.assertEqual(payload["review"]["status"], "approved")
            updated = json.loads(manifest_path.read_text())
            self.assertEqual(updated["review"]["reviewed_at"], "2026-06-12T12:00:00+00:00")
            self.assertEqual(updated["review"]["notes"], "looks good")
            self.assertEqual(updated["review"]["reviewer"], "daniel")
            self.assertTrue(updated["review"]["close_review_issue"])
            self.assertEqual(Path(updated["review"]["verdict_markdown"]).name, "review-verdict.md")
            self.assertEqual(Path(updated["review"]["verdict_json"]).name, "review-verdict.json")
            self.assertIn("Approved desktop video proof", updated["review"]["summary_comment"])
            self.assertEqual(writes[0][0], manifest_path.parent / "review-verdict.md")
            self.assertEqual(writes[1][0], manifest_path.parent / "review-verdict.json")
            self.assertEqual(writes[2][0], manifest_path)
            verdict_json = json.loads((manifest_path.parent / "review-verdict.json").read_text())
            self.assertEqual(verdict_json["status"], "approved")
            self.assertTrue(verdict_json["close_review_issue"])
            self.assertIn("Approved desktop video proof", (manifest_path.parent / "review-verdict.md").read_text())

    def test_review_issue_writes_local_draft_from_report_directory(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            report_dir = Path(tmpdir) / "report"
            report_dir.mkdir()
            package_path = report_dir / "review-package.json"
            package_path.write_text(json.dumps({"label": "Video Proof", "runs": []}) + "\n")
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
                    check_files=True,
                    json=False,
                ),
                desktop_review_issue_draft_fn=draft,
                atomic_write_text_fn=lambda path, text: writes.append((path, text)) or path.write_text(text),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            self.assertEqual(writes[0][0], report_dir / "github-issue.md")
            self.assertEqual(writes[1][0], report_dir / "github-issue.json")
            self.assertIn("review issue draft ready", self.printed[0])
            self.assertIn("attachments: 1", self.printed[-3])

    def test_review_issue_reports_missing_package(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            result = self.mod.cmd_desktop_review_issue(
                Namespace(
                    path=str(Path(tmpdir) / "missing"),
                    title=None,
                    repo=None,
                    body_output=None,
                    json_output=None,
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
                    check_files=True,
                    json=True,
                ),
                desktop_review_issue_draft_fn=fail_file_check,
                atomic_write_text_fn=lambda path, text: self.fail("draft should not be written"),
                print_fn=self.print_line,
            )

        self.assertEqual(result, 1)
        self.assertEqual(self.printed[-1], "Error: run 1 attachment missing: proof.issue.mp4")

    def test_verdict_records_needs_work_and_missing_manifest_error(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            manifest_path = Path(tmpdir) / "manifest.json"
            manifest_path.write_text(json.dumps({"label": "video-proof"}) + "\n")

            result = self.mod.cmd_desktop_verdict(
                Namespace(
                    manifest=str(manifest_path),
                    approved=False,
                    needs_work=True,
                    notes="zoom starts too late",
                    reviewer="",
                    issue_url="",
                    json=False,
                ),
                now_iso_fn=lambda: "2026-06-12T13:00:00+00:00",
                atomic_write_text_fn=lambda path, text: path.write_text(text),
                print_fn=self.print_line,
            )
            self.assertEqual(result, 0)
            updated = json.loads(manifest_path.read_text())
            self.assertEqual(updated["review"]["status"], "needs-work")
            self.assertTrue(updated["review"]["follow_up_required"])
            self.assertFalse(updated["review"]["close_review_issue"])
            self.assertIn("zoom starts too late", updated["review"]["follow_up"]["text"])
            self.assertIn("Keep the review issue open", (manifest_path.parent / "review-verdict.md").read_text())
            verdict_json = json.loads((manifest_path.parent / "review-verdict.json").read_text())
            self.assertEqual(verdict_json["follow_up"]["kind"], "same-issue-checklist")
            self.assertIn("needs-work", self.printed[-4])

            self.printed.clear()
            result = self.mod.cmd_desktop_verdict(
                Namespace(
                    manifest=str(Path(tmpdir) / "missing.json"),
                    approved=True,
                    needs_work=False,
                    notes="",
                    reviewer="",
                    issue_url="",
                    json=False,
                ),
                now_iso_fn=lambda: "unused",
                atomic_write_text_fn=lambda path, text: path.write_text(text),
                print_fn=self.print_line,
            )
            self.assertEqual(result, 1)
            self.assertIn("manifest not found", self.printed[-1])

        removed = []
        rollups = []
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "run"
            path.mkdir()
            result = self.mod.cmd_desktop_cleanup(
                Namespace(target="mac", older_than_days=None, keep_last=1, json=True),
                load_config_fn=lambda: {"desktop_automation": {"retention_days": 30}},
                prune_desktop_run_manifests_fn=lambda *_args, **_kwargs: [path],
                write_desktop_run_rollups_fn=lambda *args, **kwargs: rollups.append((args, kwargs)),
                desktop_cleanup_empty_line_fn=lambda: "none",
                desktop_cleanup_lines_fn=lambda paths: [f"removed {len(paths)}"],
                remove_tree_fn=lambda remove_path, **_kwargs: removed.append(remove_path),
                print_fn=self.print_line,
            )
        self.assertEqual(result, 0)
        self.assertEqual(removed, [path])
        self.assertEqual(len(rollups), 2)
        self.assertEqual(json.loads(self.printed[-1])["removed"], [str(path)])

    def test_recent_publish_cleanup_empty_and_error_paths(self):
        config = {"desktop_automation": {"retention_days": 30}}
        result = self.mod.cmd_desktop_recent(
            Namespace(target=None, action=None, limit=5, json=False),
            load_config_fn=lambda: (_ for _ in ()).throw(FileNotFoundError("missing desktop config")),
            desktop_run_manifests_fn=lambda *_args, **_kwargs: [],
            desktop_run_summary_fn=lambda _config, manifest: manifest,
            desktop_recent_lines_fn=lambda summaries, **_kwargs: [],
            short_sha_fn=lambda value: value[:12],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 1)
        self.assertEqual(self.printed[-1], "Error: missing desktop config")

        result = self.mod.cmd_desktop_publish(
            Namespace(target=None, action=None, limit=5, output=None, label=None, json=False),
            load_config_fn=lambda: config,
            desktop_run_manifests_fn=lambda *_args, **_kwargs: [],
            stage_desktop_publish_report_fn=lambda *_args, **_kwargs: {},
            desktop_publish_lines_fn=lambda _report: [],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.printed[-1], "No desktop automation runs found.")

        result = self.mod.cmd_desktop_cleanup(
            Namespace(target=None, older_than_days=None, keep_last=1, json=False),
            load_config_fn=lambda: config,
            prune_desktop_run_manifests_fn=lambda *_args, **_kwargs: [],
            write_desktop_run_rollups_fn=lambda *_args, **_kwargs: None,
            desktop_cleanup_empty_line_fn=lambda: "Desktop cleanup: nothing to remove.",
            desktop_cleanup_lines_fn=lambda _paths: [],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.printed[-1], "Desktop cleanup: nothing to remove.")


if __name__ == "__main__":
    unittest.main()
