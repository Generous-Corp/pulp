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


MODULE_PATH = Path(__file__).resolve().with_name("simulator_video_commands_cli.py")


def load_module():
    sys.path.insert(0, str(MODULE_PATH.parent))
    spec = importlib.util.spec_from_file_location("simulator_video_commands_cli_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


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
            which_fn=lambda name: "/usr/bin/xcrun" if name == "xcrun" else None,
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
                Namespace(device=None, app=str(app), bundle_id=None, duration=0.1, video_fps=5.0, label="ios-proof", output=str(output), json=True),
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
            self.assertIn("-framerate", manifest["commands"][-1]["command"])
            self.assertIn("frame-%06d.png", manifest["commands"][-1]["frame_pattern"])
            self.assertEqual(manifest["commands"][-1]["frame_count"], 1)
            self.assertTrue(Path(payload["video"]).exists())
            self.assertIn(["/usr/bin/xcrun", "simctl", "launch", "A-UDID", "com.pulp.demo"], self.commands)


if __name__ == "__main__":
    unittest.main()
