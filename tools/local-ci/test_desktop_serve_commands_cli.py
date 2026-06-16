#!/usr/bin/env python3
"""Tests for the desktop report serve command."""

from argparse import Namespace
import subprocess
import tempfile
from pathlib import Path
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_serve_commands_cli.py")


class DesktopServeCommandsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.printed: list[str] = []

    def print_line(self, line: str):
        self.printed.append(line)

    def _config(self, root: str):
        return {"desktop_automation": {"artifact_root": root}}

    def _args(self, **over):
        base = dict(path=None, host="127.0.0.1", port=8765, auto_port=False, background=False,
                    label="demo", status=False, stop=False, json=False)
        base.update(over)
        return Namespace(**base)

    def test_candidate_urls_include_tailscale_and_configured_hosts(self):
        def run(cmd, **kwargs):
            return subprocess.CompletedProcess(cmd, 0, stdout="100.64.0.20\n100.64.0.21\n", stderr="")

        urls = self.mod.desktop_serve_candidate_urls(
            "0.0.0.0", 8765,
            run_fn=run,
            which_fn=lambda name: "/usr/bin/tailscale" if name == "tailscale" else None,
            environ={"PULP_DESKTOP_SERVE_HOSTS": "blackbook.tailnet.ts.net, 100.64.0.20"},
            hostname_fn=lambda: "macstudio",
        )
        self.assertIn("http://127.0.0.1:8765/", urls)
        self.assertIn("http://100.64.0.21:8765/", urls)

    def test_candidate_urls_loopback_bind_avoids_public_hosts(self):
        urls = self.mod.desktop_serve_candidate_urls(
            "127.0.0.1", 8765,
            run_fn=lambda *_a, **_k: self.fail("tailscale should not be queried for loopback bind"),
            which_fn=lambda _n: "/usr/bin/tailscale",
            environ={"PULP_DESKTOP_SERVE_HOSTS": "blackbook.tailnet.ts.net"},
            hostname_fn=lambda: "macstudio",
        )
        self.assertEqual(urls, ["http://127.0.0.1:8765/"])

    def test_serve_stop_returns_zero_when_stopped(self):
        with tempfile.TemporaryDirectory() as root:
            rc = self.mod.cmd_desktop_serve(
                self._args(stop=True),
                load_config_fn=lambda: self._config(root),
                desktop_publish_reports_fn=lambda *_a, **_k: [],
                stop_serve_process_fn=lambda pr, label: {"status": "stopped", "pid": 99, "label": label},
                print_fn=self.print_line,
            )
            self.assertEqual(rc, 0)
            self.assertTrue(any("stopped" in line for line in self.printed))

    def test_serve_status_missing(self):
        with tempfile.TemporaryDirectory() as root:
            rc = self.mod.cmd_desktop_serve(
                self._args(status=True, json=True),
                load_config_fn=lambda: self._config(root),
                desktop_publish_reports_fn=lambda *_a, **_k: [],
                read_serve_state_fn=lambda pr, label: None,
                print_fn=self.print_line,
            )
            self.assertEqual(rc, 0)
            self.assertTrue(any("missing" in line for line in self.printed))

    def test_serve_status_running(self):
        with tempfile.TemporaryDirectory() as root:
            rc = self.mod.cmd_desktop_serve(
                self._args(status=True),
                load_config_fn=lambda: self._config(root),
                desktop_publish_reports_fn=lambda *_a, **_k: [],
                read_serve_state_fn=lambda pr, label: {"pid": 123, "urls": ["http://x/"], "directory": "/d"},
                is_running_fn=lambda pid: True,
                print_fn=self.print_line,
            )
            self.assertEqual(rc, 0)
            self.assertTrue(any("running" in line for line in self.printed))

    def test_serve_foreground_serves_report_dir(self):
        with tempfile.TemporaryDirectory() as root:
            publish_root = Path(root) / "_published"
            report = publish_root / "report"
            report.mkdir(parents=True)
            (report / "index.html").write_text("<html></html>")
            served = {}

            rc = self.mod.cmd_desktop_serve(
                self._args(path=str(report)),
                load_config_fn=lambda: self._config(root),
                desktop_publish_reports_fn=lambda *_a, **_k: [],
                desktop_serve_candidate_urls_fn=lambda host, port: [f"http://{host}:{port}/"],
                persist_serve_urls_fn=lambda *a, **k: None,
                serve_directory_fn=lambda path, *, host, port: served.update({"path": path, "host": host, "port": port}),
                print_fn=self.print_line,
            )
            self.assertEqual(rc, 0)
            self.assertEqual(served["path"], report)

    def test_serve_rejects_dir_outside_publish_root(self):
        with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as outside:
            rc = self.mod.cmd_desktop_serve(
                self._args(path=outside),
                load_config_fn=lambda: self._config(root),
                desktop_publish_reports_fn=lambda *_a, **_k: [],
                print_fn=self.print_line,
            )
            self.assertEqual(rc, 1)
            self.assertTrue(any("publish root" in line for line in self.printed))


if __name__ == "__main__":
    unittest.main()
