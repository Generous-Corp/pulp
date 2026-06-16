#!/usr/bin/env python3
from __future__ import annotations

import json
from argparse import Namespace
from pathlib import Path
import subprocess
import tempfile
import unittest


from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("simulator_video_commands_cli.py", add_module_dir=True)


class SimulatorVideoCommandsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.printed: list[str] = []
        self.commands: list[list[str]] = []

    def print_line(self, line: str):
        self.printed.append(line)

    def run_fn(self, command, **kwargs):
        self.commands.append(list(command))
        if command[:4] == ["/usr/bin/xcrun", "simctl", "list", "devices"]:
            return subprocess.CompletedProcess(
                command,
                0,
                stdout=json.dumps(
                    {
                        "devices": {
                            "com.apple.CoreSimulator.SimRuntime.iOS-18-0": [
                                {"name": "iPhone 16", "udid": "A-UDID", "state": "Booted"}
                            ]
                        }
                    }
                ),
                stderr="",
            )
        if command[:3] == ["/usr/bin/xcrun", "simctl", "install"]:
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")
        if command[:3] == ["/usr/bin/xcrun", "simctl", "launch"]:
            return subprocess.CompletedProcess(command, 0, stdout="com.pulp.demo: 1234", stderr="")
        if command[:3] == ["/usr/bin/xcrun", "simctl", "openurl"]:
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")
        if command[:5] == ["/usr/bin/xcrun", "simctl", "io", "A-UDID", "screenshot"]:
            Path(command[-1]).write_bytes(b"fake simulator png")
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="Wrote screenshot")
        if command and command[0] == "/usr/bin/ffmpeg":
            Path(command[-1]).write_bytes(b"fake simulator mp4")
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")
        return subprocess.CompletedProcess(command, 1, stdout="", stderr="unexpected command")

    def test_video_doctor_reports_booted_simulator(self):
        result = self.mod.cmd_simulator_video_doctor(
            Namespace(device="iPhone 16", json=True),
            print_fn=self.print_line,
            which_fn=lambda name: f"/usr/bin/{name}" if name in {"xcrun", "ffmpeg"} else None,
            run_fn=self.run_fn,
        )

        payload = json.loads(self.printed[0])
        self.assertEqual(result, 0)
        self.assertTrue(payload["ok"])
        self.assertEqual(payload["booted_devices"][0]["udid"], "A-UDID")

    def test_video_doctor_reports_missing_xcrun(self):
        result = self.mod.cmd_simulator_video_doctor(
            Namespace(device=None, json=False),
            print_fn=self.print_line,
            which_fn=lambda _name: None,
            run_fn=self.run_fn,
        )

        self.assertEqual(result, 1)
        self.assertIn("FAIL xcrun", "\n".join(self.printed))
        self.assertIn("xcode-select --install", "\n".join(self.printed))

    def test_simulator_video_records_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = Path(tmp) / "PulpDemo.app"
            app.mkdir()
            (app / "Info.plist").write_bytes(
                b"""<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\"><dict><key>CFBundleIdentifier</key><string>com.pulp.demo</string></dict></plist>
"""
            )
            output = Path(tmp) / "run"

            result = self.mod.cmd_simulator_video(
                Namespace(
                    device=None,
                    app=str(app),
                    bundle_id=None,
                    open_url="pulp-demo://toggle",
                    action_after=0.0,
                    action_label="toggle deep link",
                    duration=0.1,
                    video_fps=5.0,
                    label="ios-proof",
                    output=str(output),
                    json=True,
                ),
                print_fn=self.print_line,
                which_fn=lambda name: f"/usr/bin/{name}" if name in {"xcrun", "ffmpeg"} else None,
                run_fn=self.run_fn,
                time_sleep_fn=lambda _secs: None,
            )

            self.assertEqual(result, 0)
            payload = json.loads(self.printed[0])
            manifest = json.loads(Path(payload["manifest"]).read_text())
            self.assertEqual(manifest["target"], "ios-simulator")
            self.assertEqual(manifest["simulator"]["udid"], "A-UDID")
            self.assertEqual(manifest["app"]["bundle_id"], "com.pulp.demo")
            self.assertEqual(manifest["video"]["template"], "mobile-simulator")
            self.assertEqual(manifest["video"]["fps"], 5.0)
            self.assertEqual(manifest["video"]["recorder"], "xcrun simctl io screenshot + ffmpeg")
            record_step = next(item for item in manifest["commands"] if item["step"] == "record-video")
            self.assertIn("-framerate", record_step["command"])
            self.assertIn("frame-%06d.png", record_step["frame_pattern"])
            self.assertEqual(record_step["frame_count"], 1)
            self.assertEqual(manifest["simulator_action"]["kind"], "open-url")
            self.assertEqual(manifest["simulator_action"]["url"], "pulp-demo://toggle")
            self.assertEqual(manifest["interaction"]["mode"], "open-url")
            self.assertEqual(manifest["interaction"]["label"], "toggle deep link")
            self.assertEqual(manifest["video_proof_composition"]["template"], "mobile-simulator")
            self.assertEqual(manifest["video_proof_composition"]["action_marker"]["kind"], "open-url")
            self.assertEqual(manifest["video_proof_composition"]["action_marker"]["label"], "toggle deep link")
            self.assertIn("simulator device/runtime", manifest["video_proof_notes"][0])
            self.assertIn("simctl open-url", manifest["video_proof_composition"]["notes"][1])
            self.assertTrue(Path(payload["video"]).exists())
            self.assertIn(["/usr/bin/xcrun", "simctl", "launch", "A-UDID", "com.pulp.demo"], self.commands)
            self.assertIn(["/usr/bin/xcrun", "simctl", "openurl", "A-UDID", "pulp-demo://toggle"], self.commands)

    def test_simulator_video_can_compose_issue_ready_proof(self):
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

            result = self.mod.cmd_simulator_video(
                Namespace(
                    device=None,
                    app=None,
                    bundle_id=None,
                    open_url="https://example.com",
                    action_after=0.0,
                    action_label="open example.com",
                    duration=0.1,
                    video_fps=5.0,
                    compose_video_proof=True,
                    video_title="Simulator proof",
                    video_note=["URL opened during capture"],
                    video_attachment_budget_mb=25.0,
                    small_video=True,
                    small_video_budget_mb=8.0,
                    label="ios-proof",
                    output=str(output),
                    json=True,
                ),
                print_fn=self.print_line,
                which_fn=lambda name: f"/usr/bin/{name}" if name in {"xcrun", "ffmpeg"} else None,
                run_fn=self.run_fn,
                time_sleep_fn=lambda _secs: None,
                compose_mobile_video_proof_fn=compose,
            )

            self.assertEqual(result, 0)
            payload = json.loads(self.printed[0])
            manifest = json.loads(Path(payload["manifest"]).read_text())
            self.assertEqual(payload["composition"]["artifacts"]["video_issue"], str(output / "video" / "proof.issue.mp4"))
            self.assertEqual(compose_calls[0]["template"], "mobile-simulator")
            self.assertEqual(compose_calls[0]["title"], "Simulator proof")
            self.assertEqual(compose_calls[0]["notes"], ["URL opened during capture"])
            self.assertEqual(compose_calls[0]["video_attachment_budget_mb"], 25.0)
            self.assertTrue(compose_calls[0]["small_video"])
            self.assertEqual(compose_calls[0]["small_video_budget_mb"], 8.0)
            self.assertEqual(manifest["video_proof_composition"]["template"], "mobile-simulator")


if __name__ == "__main__":
    unittest.main()
