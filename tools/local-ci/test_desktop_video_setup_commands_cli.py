#!/usr/bin/env python3
"""Tests for the desktop video-doctor / video-setup commands."""

from __future__ import annotations

import json
import os
from argparse import Namespace
from pathlib import Path
import subprocess
import tempfile
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_video_setup_commands_cli.py", add_module_dir=True)


class DesktopVideoSetupCommandsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.printed: list[str] = []
        self.writes: list[tuple[Path, str]] = []
        self.saved_configs: list[dict] = []
        self.targets = {
            "mac": {
                "adapter": "macos-local",
                "bootstrap": "launchagent",
                "target_type": "local",
                "capability_tier": "full",
            },
            "ubuntu": {
                "adapter": "linux-xvfb",
                "bootstrap": "xvfb-run",
                "target_type": "ssh",
                "capability_tier": "full",
                "host": "ubuntu",
            },
            "windows": {
                "adapter": "windows-session-agent",
                "bootstrap": "scheduled-task",
                "target_type": "ssh",
                "capability_tier": "full",
                "host": "win",
                "repo_path": r"C:\Old\Pulp",
            },
        }

    def print_line(self, line: str):
        self.printed.append(line)

    def config(self):
        return {
            "defaults": {},
            "desktop_automation": {
                "artifact_root": "/tmp/desktop-artifacts",
                "targets": self.targets,
            },
        }

    def setup_ok_checks(self):
        return [
            {"name": "setup.pulp", "ok": True, "detail": "/usr/local/bin/pulp", "required": True, "title": "Pulp CLI"},
            {"name": "setup.npm", "ok": True, "detail": "/usr/local/bin/npm", "required": True, "title": "npm"},
            {"name": "setup.node", "ok": True, "detail": "/usr/local/bin/node", "required": True, "title": "Node.js"},
            {"name": "setup.cmake", "ok": True, "detail": "/usr/local/bin/cmake", "required": True, "title": "CMake"},
        ]

    def deps(self):
        def resolve(_config, name):
            return self.targets[name]

        def update_repo_path(config, name, repo_path):
            config["desktop_automation"]["targets"][name]["repo_path"] = repo_path
            self.targets[name]["repo_path"] = repo_path

        return {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": resolve,
            "check_writable_dir_fn": lambda _path: (True, ""),
            "desktop_target_contract_fn": lambda name, target: {
                "target": name,
                "task_name": f"PulpDesktopAutomationAgent-{name}",
                "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent"
                if target["adapter"] == "windows-session-agent"
                else None,
            },
            "ensure_host_reachable_fn": lambda _name, target, _defaults: target.get("host"),
            "bootstrap_windows_session_agent_fn": lambda _host, _contract: {
                "remote_root": r"C:\RemoteRoot",
                "script_path": r"C:\RemoteRoot\agent.ps1",
            },
            "probe_windows_session_agent_fn": lambda _host, _contract: {
                "task_present": True,
                "agent_root_exists": True,
                "jobs_dir_exists": True,
                "results_dir_exists": True,
                "script_exists": True,
            },
            "subprocess_run_fn": lambda *_args, **_kwargs: subprocess.CompletedProcess(["git"], 0, "a" * 40 + "\n", ""),
            "root_path": Path("/repo"),
            "new_install_job_id_fn": lambda: "install123",
            "sync_job_bundle_to_ssh_host_fn": lambda _host, job: (f"{job['id']}.git", "refs/pulp-ci-bundles/install"),
            "ensure_windows_remote_tooling_fn": lambda _host: {
                "probe": {
                    "git_found": True,
                    "git_path": r"C:\Program Files\Git\cmd\git.exe",
                    "git_version": "git version 2.49.0.windows.1",
                },
                "installed": ["git"],
            },
            "windows_remote_tooling_ready_fn": lambda probe: bool(probe.get("git_found")),
            "ensure_windows_remote_repo_checkout_fn": lambda *_args, **_kwargs: {
                "repo_path": r"C:\Users\daniel\pulp-validate",
                "repo_exists": True,
            },
            "git_origin_clone_url_fn": lambda _root: "https://github.com/danielraffel/pulp",
            "windows_repo_checkout_ready_fn": lambda probe: bool(probe.get("repo_exists")),
            "update_target_repo_path_fn": update_repo_path,
            "save_config_fn": self.saved_configs.append,
            "now_iso_fn": lambda: "2026-06-11T00:00:00Z",
            "desktop_target_receipt_path_fn": lambda name: Path(f"/receipts/{name}.json"),
            "atomic_write_text_fn": lambda path, text: self.writes.append((path, text)),
            "windows_tooling_detail_fn": lambda probe, tool_name, **_kwargs: f"{probe.get(tool_name + '_version')} ({probe.get(tool_name + '_path')})",
            "print_fn": self.print_line,
        }

    def written_receipt(self):
        self.assertEqual(len(self.writes), 1)
        return json.loads(self.writes[0][1])

    def test_video_doctor_requires_video_capture_and_can_skip_remotion_smoke(self):
        self.targets["mac"]["optional"] = {"video_capture": True}
        checks = [
            {"name": "receipt", "ok": True, "detail": "installed"},
            {"name": "screencapture", "ok": True, "detail": "/usr/sbin/screencapture"},
            {"name": "video_capture", "ok": True, "detail": "/repo/node_modules/ffmpeg-static/ffmpeg", "required": False},
            {"name": "avfoundation_screen", "ok": True, "detail": "Capture screen 0 (3:)", "required": False},
        ]
        deps = {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": lambda _config, name: self.targets[name],
            "desktop_doctor_checks_fn": lambda _config, _name: [dict(check) for check in checks],
            "normalize_desktop_optional_config_fn": lambda optional: {"video_capture": bool((optional or {}).get("video_capture"))},
            "video_proof_smoke_fn": lambda: self.fail("smoke should be skipped"),
            "probe_macos_avfoundation_audio_fn": lambda _device: self.fail("audio probe should not run"),
            "print_fn": self.print_line,
        }

        result = self.mod.cmd_desktop_video_doctor(
            Namespace(target="mac", json=False, skip_remotion_smoke=True, video_audio="none", video_audio_device=None),
            **deps,
        )

        self.assertEqual(result, 0)
        self.assertIn("Desktop video doctor for `mac`", self.printed)
        self.assertIn("  PASS  video_capture: /repo/node_modules/ffmpeg-static/ffmpeg", self.printed)
        self.assertIn("  PASS  avfoundation_screen: Capture screen 0 (3:)", self.printed)
        self.assertIn("  PASS  backend.recorder: macOS ffmpeg/AVFoundation recorder with screencapture fallback", self.printed)
        self.assertIn("  PASS  target.video_capture: enabled", self.printed)
        self.assertIn("  PASS  remotion_smoke: skipped by --skip-remotion-smoke", self.printed)
        self.assertNotIn("Remediation:", self.printed)

    def test_video_doctor_fails_unsupported_desktop_recorder_backend(self):
        self.targets["windows"]["optional"] = {"video_capture": True}
        checks = [
            {"name": "receipt", "ok": True, "detail": "installed"},
            {"name": "video_capture", "ok": True, "detail": "/repo/node_modules/ffmpeg-static/ffmpeg", "required": False},
        ]
        deps = {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": lambda _config, name: self.targets[name],
            "desktop_doctor_checks_fn": lambda _config, _name: [dict(check) for check in checks],
            "normalize_desktop_optional_config_fn": lambda optional: {"video_capture": bool((optional or {}).get("video_capture"))},
            "video_proof_smoke_fn": lambda: {"ok": True, "detail": "ok"},
            "probe_macos_avfoundation_audio_fn": lambda _device: self.fail("audio probe should not run"),
            "print_fn": self.print_line,
        }

        result = self.mod.cmd_desktop_video_doctor(
            Namespace(target="windows", json=True, skip_remotion_smoke=False, video_audio="none", video_audio_device=None),
            **deps,
        )

        self.assertEqual(result, 1)
        payload = json.loads(self.printed[0])
        checks_by_name = {check["name"]: check for check in payload["checks"]}
        self.assertFalse(payload["ok"])
        self.assertFalse(checks_by_name["backend.recorder"]["ok"])
        self.assertTrue(checks_by_name["backend.recorder"]["required"])
        self.assertIn("ffmpeg ddagrab/gdigrab", checks_by_name["backend.recorder"]["detail"])
        remediations_by_check = {item["check"]: item for item in payload["remediations"]}
        self.assertIn("not implemented yet", remediations_by_check["backend.recorder"]["detail"])

    def test_video_doctor_reports_config_and_remotion_failures(self):
        checks = [
            {"name": "receipt", "ok": True, "detail": "installed"},
            {"name": "video_capture", "ok": False, "detail": "ffmpeg not found", "required": False},
            {"name": "avfoundation_screen", "ok": False, "detail": "Could not find AVFoundation device `Capture screen 0`", "required": False},
        ]
        deps = {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": lambda _config, name: self.targets[name],
            "desktop_doctor_checks_fn": lambda _config, _name: [dict(check) for check in checks],
            "normalize_desktop_optional_config_fn": lambda optional: {"video_capture": bool((optional or {}).get("video_capture"))},
            "video_proof_smoke_fn": lambda: {"ok": False, "detail": "npm install required"},
            "probe_macos_avfoundation_audio_fn": lambda _device: self.fail("audio probe should not run"),
            "print_fn": self.print_line,
        }

        result = self.mod.cmd_desktop_video_doctor(
            Namespace(target="mac", json=True, skip_remotion_smoke=False, video_audio="none", video_audio_device=None),
            **deps,
        )

        self.assertEqual(result, 1)
        payload = json.loads(self.printed[0])
        checks_by_name = {check["name"]: check for check in payload["checks"]}
        self.assertFalse(payload["ok"])
        self.assertFalse(checks_by_name["target.video_capture"]["ok"])
        self.assertTrue(checks_by_name["target.video_capture"]["required"])
        self.assertFalse(checks_by_name["video_capture"]["ok"])
        self.assertTrue(checks_by_name["video_capture"]["required"])
        self.assertFalse(checks_by_name["avfoundation_screen"]["ok"])
        self.assertTrue(checks_by_name["avfoundation_screen"]["required"])
        self.assertFalse(checks_by_name["remotion_smoke"]["ok"])
        self.assertEqual(checks_by_name["remotion_smoke"]["detail"], "npm install required")
        remediations_by_check = {item["check"]: item for item in payload["remediations"]}
        self.assertEqual(remediations_by_check["target.video_capture"]["command"], "python3 tools/local-ci/local_ci.py desktop config set target.mac.video_capture true")
        self.assertEqual(remediations_by_check["video_capture"]["command"], "npm --prefix tools/local-ci install")
        self.assertEqual(remediations_by_check["video_capture"]["future_command"], "pulp tool install video-proof")
        self.assertEqual(remediations_by_check["video_capture"]["future_check_command"], "pulp tool doctor video-proof --run")
        self.assertEqual(remediations_by_check["avfoundation_screen"]["command"], "python3 tools/local-ci/local_ci.py desktop video-doctor mac --json")
        self.assertEqual(remediations_by_check["remotion_smoke"]["command"], "npm --prefix tools/local-ci run smoke-video-proof")
        self.assertEqual(remediations_by_check["remotion_smoke"]["future_command"], "pulp tool doctor video-proof --run")
        self.assertEqual(payload["install_model"]["current_command"], "npm --prefix tools/local-ci install")
        self.assertEqual(payload["install_model"]["future_command"], "pulp tool install video-proof")
        self.assertEqual(payload["install_model"]["future_check_command"], "pulp tool doctor video-proof --run")
        self.assertEqual(payload["install_model"]["pack_command"], "python3 tools/local-ci/pack_video_proof_tool.py --json")
        self.assertEqual(payload["install_model"]["pack_npm_script"], "npm --prefix tools/local-ci run pack-video-proof-tool -- --json")
        self.assertEqual(payload["install_model"]["verify_command"], "python3 tools/local-ci/pack_video_proof_tool.py --verify <manifest> --json")
        self.assertEqual(payload["install_model"]["artifact_install_command"], "pulp tool install video-proof --artifact-manifest <manifest> --force")
        self.assertEqual(payload["install_model"]["pack_manifest_schema"], "pulp.video-proof-tool-package.v1")

    def test_video_doctor_reports_screen_recording_remediation(self):
        self.targets["mac"]["optional"] = {"video_capture": True}
        checks = [
            {"name": "receipt", "ok": True, "detail": "installed"},
            {"name": "screencapture", "ok": False, "detail": "could not create image from display"},
            {"name": "video_capture", "ok": True, "detail": "/repo/node_modules/ffmpeg-static/ffmpeg", "required": False},
            {"name": "avfoundation_screen", "ok": True, "detail": "Capture screen 0 (3:)", "required": False},
        ]
        deps = {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": lambda _config, name: self.targets[name],
            "desktop_doctor_checks_fn": lambda _config, _name: [dict(check) for check in checks],
            "normalize_desktop_optional_config_fn": lambda optional: {"video_capture": bool((optional or {}).get("video_capture"))},
            "video_proof_smoke_fn": lambda: {"ok": True, "detail": "smoke ok"},
            "probe_macos_avfoundation_audio_fn": lambda _device: self.fail("audio probe should not run"),
            "print_fn": self.print_line,
        }

        result = self.mod.cmd_desktop_video_doctor(
            Namespace(target="mac", json=False, skip_remotion_smoke=False, video_audio="none", video_audio_device=None),
            **deps,
        )

        self.assertEqual(result, 1)
        self.assertIn("  FAIL  screencapture: could not create image from display", self.printed)
        self.assertIn("Remediation:", self.printed)
        self.assertTrue(any("Grant macOS Screen Recording permission" in line for line in self.printed))
        self.assertTrue(any("Privacy_ScreenCapture" in line for line in self.printed))
        self.assertTrue(any("--run-in-terminal --json" in line for line in self.printed))

        remediation = self.mod.desktop_video_doctor_remediations(checks, target_name="mac")[0]
        self.assertEqual(
            remediation["rerun_command"],
            "python3 tools/local-ci/local_ci.py desktop video-doctor mac --run-in-terminal --json",
        )
        self.assertIn("Terminal.app", remediation["detail"])

    def test_video_doctor_reports_receipt_remediation(self):
        self.targets["mac"]["optional"] = {"video_capture": True}
        checks = [
            {"name": "receipt", "ok": False, "detail": "not installed"},
            {"name": "screencapture", "ok": True, "detail": "/usr/sbin/screencapture"},
            {"name": "video_capture", "ok": True, "detail": "/repo/node_modules/ffmpeg-static/ffmpeg", "required": False},
            {"name": "avfoundation_screen", "ok": True, "detail": "Capture screen 0 (3:)", "required": False},
        ]
        deps = {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": lambda _config, name: self.targets[name],
            "desktop_doctor_checks_fn": lambda _config, _name: [dict(check) for check in checks],
            "normalize_desktop_optional_config_fn": lambda optional: {"video_capture": bool((optional or {}).get("video_capture"))},
            "video_proof_smoke_fn": lambda: {"ok": True, "detail": "smoke ok"},
            "probe_macos_avfoundation_audio_fn": lambda _device: self.fail("audio probe should not run"),
            "print_fn": self.print_line,
        }

        result = self.mod.cmd_desktop_video_doctor(
            Namespace(target="mac", json=True, skip_remotion_smoke=False, video_audio="none", video_audio_device=None),
            **deps,
        )

        self.assertEqual(result, 1)
        payload = json.loads(self.printed[0])
        remediations_by_check = {item["check"]: item for item in payload["remediations"]}
        self.assertEqual(remediations_by_check["receipt"]["command"], "python3 tools/local-ci/local_ci.py desktop install mac")

    def test_video_doctor_allows_screencapture_fallback_when_avfoundation_is_hidden(self):
        self.targets["mac"]["optional"] = {"video_capture": True}
        checks = [
            {"name": "receipt", "ok": True, "detail": "installed"},
            {"name": "screencapture", "ok": True, "detail": "/usr/sbin/screencapture"},
            {"name": "video_capture", "ok": True, "detail": "/repo/node_modules/ffmpeg-static/ffmpeg", "required": False},
            {"name": "avfoundation_screen", "ok": False, "detail": "Could not find AVFoundation device `Capture screen 0`", "required": False},
        ]
        deps = {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": lambda _config, name: self.targets[name],
            "desktop_doctor_checks_fn": lambda _config, _name: [dict(check) for check in checks],
            "normalize_desktop_optional_config_fn": lambda optional: {"video_capture": bool((optional or {}).get("video_capture"))},
            "video_proof_smoke_fn": lambda: {"ok": True, "detail": "smoke ok"},
            "probe_macos_avfoundation_audio_fn": lambda _device: self.fail("audio probe should not run"),
            "print_fn": self.print_line,
        }

        result = self.mod.cmd_desktop_video_doctor(
            Namespace(target="mac", json=True, skip_remotion_smoke=False, video_audio="none", video_audio_device=None),
            **deps,
        )

        self.assertEqual(result, 0)
        payload = json.loads(self.printed[0])
        checks_by_name = {check["name"]: check for check in payload["checks"]}
        self.assertTrue(payload["ok"])
        self.assertFalse(checks_by_name["avfoundation_screen"]["ok"])
        self.assertFalse(checks_by_name["avfoundation_screen"]["required"])
        self.assertIn("screencapture fallback available", checks_by_name["avfoundation_screen"]["detail"])

    def test_video_doctor_can_validate_system_audio_device(self):
        self.targets["mac"]["optional"] = {"video_capture": True}
        audio_devices: list[str | None] = []
        checks = [
            {"name": "receipt", "ok": True, "detail": "installed"},
            {"name": "screencapture", "ok": True, "detail": "/usr/sbin/screencapture"},
            {"name": "video_capture", "ok": True, "detail": "/repo/node_modules/ffmpeg-static/ffmpeg", "required": False},
            {"name": "avfoundation_screen", "ok": True, "detail": "Capture screen 0 (3:)", "required": False},
        ]
        deps = {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": lambda _config, name: self.targets[name],
            "desktop_doctor_checks_fn": lambda _config, _name: [dict(check) for check in checks],
            "normalize_desktop_optional_config_fn": lambda optional: {"video_capture": bool((optional or {}).get("video_capture"))},
            "video_proof_smoke_fn": lambda: {"ok": True, "detail": "smoke ok"},
            "probe_macos_avfoundation_audio_fn": lambda device: audio_devices.append(device) or (True, "BlackHole 2ch (2)"),
            "print_fn": self.print_line,
        }

        result = self.mod.cmd_desktop_video_doctor(
            Namespace(target="mac", json=True, skip_remotion_smoke=False, video_audio="system", video_audio_device="2"),
            **deps,
        )

        self.assertEqual(result, 0)
        payload = json.loads(self.printed[0])
        checks_by_name = {check["name"]: check for check in payload["checks"]}
        self.assertEqual(audio_devices, ["2"])
        self.assertTrue(checks_by_name["avfoundation_audio"]["ok"])
        self.assertEqual(checks_by_name["avfoundation_audio"]["detail"], "BlackHole 2ch (2)")

    def test_video_doctor_reports_system_audio_device_remediation(self):
        self.targets["mac"]["optional"] = {"video_capture": True}
        checks = [
            {"name": "receipt", "ok": True, "detail": "installed"},
            {"name": "screencapture", "ok": True, "detail": "/usr/sbin/screencapture"},
            {"name": "video_capture", "ok": True, "detail": "/repo/node_modules/ffmpeg-static/ffmpeg", "required": False},
            {"name": "avfoundation_screen", "ok": True, "detail": "Capture screen 0 (3:)", "required": False},
        ]
        deps = {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": lambda _config, name: self.targets[name],
            "desktop_doctor_checks_fn": lambda _config, _name: [dict(check) for check in checks],
            "normalize_desktop_optional_config_fn": lambda optional: {"video_capture": bool((optional or {}).get("video_capture"))},
            "video_proof_smoke_fn": lambda: {"ok": True, "detail": "smoke ok"},
            "probe_macos_avfoundation_audio_fn": lambda _device: (False, "No AVFoundation audio device configured"),
            "print_fn": self.print_line,
        }

        result = self.mod.cmd_desktop_video_doctor(
            Namespace(target="mac", json=True, skip_remotion_smoke=False, video_audio="system", video_audio_device=None),
            **deps,
        )

        self.assertEqual(result, 1)
        payload = json.loads(self.printed[0])
        checks_by_name = {check["name"]: check for check in payload["checks"]}
        remediations_by_check = {item["check"]: item for item in payload["remediations"]}
        self.assertFalse(checks_by_name["avfoundation_audio"]["ok"])
        self.assertEqual(remediations_by_check["avfoundation_audio"]["check"], "avfoundation_audio")
        self.assertIn("PULP_VIDEO_AUDIO_DEVICE", remediations_by_check["avfoundation_audio"]["command"])

    def test_video_doctor_reports_reaper_stale_clap_cache(self):
        self.targets["mac"]["optional"] = {"video_capture": True}
        self.mod.reaper_video_recipe.installed_clap_bundle_status = lambda _plugin: (True, "CLAP bundle executable found")
        self.mod.reaper_video_recipe.reaper_clap_cache_status = lambda _plugin: (False, "cache stanza exists but no plugin descriptor")
        checks = [
            {"name": "receipt", "ok": True, "detail": "installed"},
            {"name": "screencapture", "ok": True, "detail": "/usr/sbin/screencapture"},
            {"name": "video_capture", "ok": True, "detail": "/repo/node_modules/ffmpeg-static/ffmpeg", "required": False},
            {"name": "avfoundation_screen", "ok": True, "detail": "Capture screen 0 (3:)", "required": False},
        ]
        deps = {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": lambda _config, name: self.targets[name],
            "desktop_doctor_checks_fn": lambda _config, _name: [dict(check) for check in checks],
            "normalize_desktop_optional_config_fn": lambda optional: {"video_capture": bool((optional or {}).get("video_capture"))},
            "video_proof_smoke_fn": lambda: {"ok": True, "detail": "smoke ok"},
            "probe_macos_avfoundation_audio_fn": lambda _device: self.fail("audio probe should not run"),
            "print_fn": self.print_line,
        }

        result = self.mod.cmd_desktop_video_doctor(
            Namespace(
                target="mac",
                json=True,
                skip_remotion_smoke=False,
                video_audio="none",
                video_audio_device=None,
                recipe="reaper-plugin-editor",
                plugin="PulpSynth",
                plugin_format="clap",
            ),
            **deps,
        )

        self.assertEqual(result, 1)
        payload = json.loads(self.printed[0])
        checks_by_name = {check["name"]: check for check in payload["checks"]}
        remediations_by_check = {item["check"]: item for item in payload["remediations"]}
        self.assertTrue(checks_by_name["recipe.reaper"]["ok"])
        self.assertTrue(checks_by_name["reaper.clap_bundle"]["ok"])
        self.assertFalse(checks_by_name["reaper.clap_cache"]["ok"])
        self.assertIn("cache stanza exists", checks_by_name["reaper.clap_cache"]["detail"])
        self.assertIn("Refresh REAPER", remediations_by_check["reaper.clap_cache"]["title"])
        self.assertEqual(remediations_by_check["reaper.clap_cache"]["command"], "open -a REAPER")

    def test_video_doctor_requires_reaper_recipe_plugin_details(self):
        self.targets["mac"]["optional"] = {"video_capture": True}
        checks = [
            {"name": "receipt", "ok": True, "detail": "installed"},
            {"name": "screencapture", "ok": True, "detail": "/usr/sbin/screencapture"},
            {"name": "video_capture", "ok": True, "detail": "/repo/node_modules/ffmpeg-static/ffmpeg", "required": False},
            {"name": "avfoundation_screen", "ok": True, "detail": "Capture screen 0 (3:)", "required": False},
        ]
        deps = {
            "load_config_fn": self.config,
            "resolve_desktop_target_fn": lambda _config, name: self.targets[name],
            "desktop_doctor_checks_fn": lambda _config, _name: [dict(check) for check in checks],
            "normalize_desktop_optional_config_fn": lambda optional: {"video_capture": bool((optional or {}).get("video_capture"))},
            "video_proof_smoke_fn": lambda: {"ok": True, "detail": "smoke ok"},
            "probe_macos_avfoundation_audio_fn": lambda _device: self.fail("audio probe should not run"),
            "print_fn": self.print_line,
        }

        result = self.mod.cmd_desktop_video_doctor(
            Namespace(
                target="mac",
                json=True,
                skip_remotion_smoke=False,
                video_audio="none",
                video_audio_device=None,
                recipe="reaper-plugin-editor",
                plugin=None,
                plugin_format=None,
            ),
            **deps,
        )

        self.assertEqual(result, 1)
        payload = json.loads(self.printed[0])
        checks_by_name = {check["name"]: check for check in payload["checks"]}
        self.assertFalse(checks_by_name["recipe.reaper"]["ok"])
        self.assertIn("requires --plugin and --plugin-format", checks_by_name["recipe.reaper"]["detail"])


if __name__ == "__main__":
    unittest.main()
