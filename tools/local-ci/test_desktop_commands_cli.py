#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
from argparse import Namespace
from pathlib import Path
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
            stage_desktop_publish_report_fn=lambda *_args, **_kwargs: {"run_count": 1},
            desktop_publish_lines_fn=lambda report: [f"published {report['run_count']}"],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.printed[-1], "published 1")

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
