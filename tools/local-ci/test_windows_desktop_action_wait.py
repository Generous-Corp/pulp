#!/usr/bin/env python3
"""No-network tests for Windows desktop session-agent result polling."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("windows_desktop_action_wait.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsDesktopActionWaitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.request = {
            "job_id": "job-123",
            "outputs": {"manifest": r"C:\Agent\results\job-123\manifest.json"},
        }

    def test_wait_for_manifest_polls_until_available(self) -> None:
        ticks = iter([0.0, 0.0, 0.5, 1.0])
        reads = []

        def read_json(_host, remote_path, **kwargs):
            reads.append((remote_path, kwargs))
            if len(reads) < 2:
                return None
            return {"status": "pass"}

        manifest = self.mod.wait_for_windows_session_agent_manifest(
            host="win-host",
            target_name="windows",
            request=self.request,
            timeout_secs=5.0,
            settle_secs=0.25,
            time_fn=lambda: next(ticks),
            sleep_fn=lambda _secs: None,
            windows_ssh_read_json_fn=read_json,
        )

        self.assertEqual(manifest, {"status": "pass"})
        self.assertEqual(len(reads), 2)
        self.assertEqual(reads[0][1], {"timeout": 15, "optional": True})

    def test_wait_for_manifest_times_out(self) -> None:
        times = iter([0.0, 20.0])
        with self.assertRaisesRegex(RuntimeError, "Timed out waiting"):
            self.mod.wait_for_windows_session_agent_manifest(
                host="win-host",
                target_name="windows",
                request=self.request,
                timeout_secs=1.0,
                settle_secs=0.0,
                time_fn=lambda: next(times),
                sleep_fn=lambda _secs: None,
                windows_ssh_read_json_fn=lambda *_args, **_kwargs: None,
            )


if __name__ == "__main__":
    unittest.main()
