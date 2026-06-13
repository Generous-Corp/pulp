#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
from argparse import Namespace
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


MODULE_PATH = Path(__file__).resolve().with_name("android_video_commands_cli.py")


def load_module():
    sys.path.insert(0, str(MODULE_PATH.parent))
    spec = importlib.util.spec_from_file_location("android_video_commands_cli_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FakeScreenrecordProcess:
    def __init__(self, command, **_kwargs):
        self.command = list(command)
        self.returncode = 0

    def communicate(self, timeout=None):
        return ("", "")


class AndroidVideoCommandsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.printed: list[str] = []
        self.commands: list[list[str]] = []
        self.popen_commands: list[list[str]] = []

    def print_line(self, line: str):
        self.printed.append(line)

    def popen_fn(self, command, **kwargs):
        self.popen_commands.append(list(command))
        return FakeScreenrecordProcess(command, **kwargs)

    def run_fn(self, command, **kwargs):
        self.commands.append(list(command))
        if command == ["/usr/bin/adb", "devices", "-l"]:
            return subprocess.CompletedProcess(
                command,
                0,
                stdout=(
                    "List of devices attached\n"
                    "emulator-5554 device product:sdk_gphone64_arm64 model:sdk_gphone64_arm64 device:emu64a transport_id:1\n"
                ),
                stderr="",
            )
        if command == ["/usr/bin/adb", "-s", "emulator-5554", "shell", "command", "-v", "screenrecord"]:
            return subprocess.CompletedProcess(command, 0, stdout="/system/bin/screenrecord\n", stderr="")
        if command[:5] == ["/usr/bin/adb", "-s", "emulator-5554", "install", "-r"]:
            return subprocess.CompletedProcess(command, 0, stdout="Success", stderr="")
        if command[:6] == ["/usr/bin/adb", "-s", "emulator-5554", "shell", "am", "start"]:
            return subprocess.CompletedProcess(command, 0, stdout="Starting: Intent", stderr="")
        if command[:5] == ["/usr/bin/adb", "-s", "emulator-5554", "shell", "monkey"]:
            return subprocess.CompletedProcess(command, 0, stdout="Events injected: 1", stderr="")
        if command[:4] == ["/usr/bin/adb", "-s", "emulator-5554", "pull"]:
            Path(command[-1]).write_bytes(b"fake android mp4")
            return subprocess.CompletedProcess(command, 0, stdout="1 file pulled", stderr="")
        if command[:6] == ["/usr/bin/adb", "-s", "emulator-5554", "shell", "rm", "-f"]:
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")
        return subprocess.CompletedProcess(command, 1, stdout="", stderr=f"unexpected command: {command}")

    def test_video_doctor_reports_connected_screenrecord_device(self):
        result = self.mod.cmd_android_video_doctor(
            Namespace(device="emulator-5554", json=True),
            print_fn=self.print_line,
            which_fn=lambda name: f"/usr/bin/{name}" if name == "adb" else None,
            run_fn=self.run_fn,
        )

        payload = json.loads(self.printed[0])
        self.assertEqual(result, 0)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["selected_device"]["serial"], "emulator-5554")
        self.assertIn("screenrecord", [check["name"] for check in payload["checks"]])

    def test_video_doctor_reports_missing_adb(self):
        result = self.mod.cmd_android_video_doctor(
            Namespace(device=None, json=False),
            print_fn=self.print_line,
            which_fn=lambda _name: None,
            run_fn=self.run_fn,
        )

        self.assertEqual(result, 1)
        self.assertIn("FAIL adb", "\n".join(self.printed))
        self.assertIn("pulp doctor android", "\n".join(self.printed))

    def test_android_video_records_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            apk = Path(tmp) / "app-debug.apk"
            apk.write_bytes(b"fake apk")
            output = Path(tmp) / "run"

            result = self.mod.cmd_android_video(
                Namespace(
                    device=None,
                    apk=str(apk),
                    package="com.pulp.demo",
                    activity=".MainActivity",
                    open_url="pulp-demo://toggle",
                    action_after=0.0,
                    action_label="toggle deep link",
                    duration=1.0,
                    label="android-proof",
                    output=str(output),
                    json=True,
                ),
                print_fn=self.print_line,
                which_fn=lambda name: f"/usr/bin/{name}" if name == "adb" else None,
                run_fn=self.run_fn,
                popen_fn=self.popen_fn,
                time_sleep_fn=lambda _secs: None,
            )

            self.assertEqual(result, 0)
            payload = json.loads(self.printed[0])
            manifest = json.loads(Path(payload["manifest"]).read_text())
            self.assertEqual(manifest["target"], "android-emulator")
            self.assertEqual(manifest["android"]["serial"], "emulator-5554")
            self.assertEqual(manifest["app"]["package"], "com.pulp.demo")
            self.assertEqual(manifest["app"]["activity"], ".MainActivity")
            self.assertEqual(manifest["video"]["template"], "mobile-emulator")
            self.assertEqual(manifest["video"]["recorder"], "adb shell screenrecord")
            self.assertEqual(manifest["interaction"]["mode"], "open-url")
            self.assertEqual(manifest["interaction"]["label"], "toggle deep link")
            self.assertEqual(manifest["android_action"]["kind"], "open-url")
            self.assertEqual(manifest["android_action"]["url"], "pulp-demo://toggle")
            self.assertEqual(manifest["video_proof_composition"]["template"], "mobile-emulator")
            self.assertEqual(manifest["video_proof_composition"]["action_marker"]["kind"], "open-url")
            self.assertIn("adb device identity", manifest["video_proof_notes"][0])
            self.assertIn("deep-link tools", manifest["video_proof_composition"]["notes"][1])
            record_step = next(item for item in manifest["commands"] if item["step"] == "record-video")
            self.assertEqual(record_step["time_limit_secs"], 1)
            self.assertTrue(Path(payload["video"]).exists())
            self.assertIn(["/usr/bin/adb", "-s", "emulator-5554", "shell", "am", "start", "-n", "com.pulp.demo/.MainActivity"], self.commands)
            self.assertIn(
                [
                    "/usr/bin/adb",
                    "-s",
                    "emulator-5554",
                    "shell",
                    "am",
                    "start",
                    "-a",
                    "android.intent.action.VIEW",
                    "-d",
                    "pulp-demo://toggle",
                ],
                self.commands,
            )
            self.assertEqual(self.popen_commands[0][:5], ["/usr/bin/adb", "-s", "emulator-5554", "shell", "screenrecord"])

    def test_android_video_can_compose_issue_ready_proof(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "run"
            compose_calls: list[dict] = []

            def compose(manifest_path: Path, **kwargs):
                compose_calls.append({"manifest": manifest_path, **kwargs})
                video_dir = manifest_path.parent / "video"
                composed = video_dir / "proof-composed.mp4"
                issue = video_dir / "proof.issue.mp4"
                small = video_dir / "proof.small.mp4"
                composed.write_bytes(b"composed")
                issue.write_bytes(b"issue")
                small.write_bytes(b"small")
                return {
                    "manifest": str(manifest_path),
                    "artifacts": {
                        "video_composed": str(composed),
                        "video_issue": str(issue),
                        "video_small": str(small),
                    },
                }

            result = self.mod.cmd_android_video(
                Namespace(
                    device=None,
                    apk=None,
                    package=None,
                    activity=None,
                    open_url="https://example.com",
                    action_after=0.0,
                    action_label="open example.com",
                    duration=1.0,
                    compose_video_proof=True,
                    video_title="Android proof",
                    video_note=["Deep link opened during capture"],
                    video_attachment_budget_mb=25.0,
                    small_video=True,
                    small_video_budget_mb=8.0,
                    label="android-proof",
                    output=str(output),
                    json=True,
                ),
                print_fn=self.print_line,
                which_fn=lambda name: f"/usr/bin/{name}" if name == "adb" else None,
                run_fn=self.run_fn,
                popen_fn=self.popen_fn,
                time_sleep_fn=lambda _secs: None,
                compose_mobile_video_proof_fn=compose,
            )

            self.assertEqual(result, 0)
            payload = json.loads(self.printed[0])
            manifest = json.loads(Path(payload["manifest"]).read_text())
            self.assertEqual(payload["composition"]["artifacts"]["video_issue"], str(output / "video" / "proof.issue.mp4"))
            self.assertEqual(compose_calls[0]["template"], "mobile-emulator")
            self.assertEqual(compose_calls[0]["title"], "Android proof")
            self.assertEqual(compose_calls[0]["notes"], ["Deep link opened during capture"])
            self.assertEqual(compose_calls[0]["video_attachment_budget_mb"], 25.0)
            self.assertTrue(compose_calls[0]["small_video"])
            self.assertEqual(compose_calls[0]["small_video_budget_mb"], 8.0)
            self.assertEqual(manifest["video_proof_composition"]["template"], "mobile-emulator")


if __name__ == "__main__":
    unittest.main()
