#!/usr/bin/env python3
"""Facade-level WebDriver probe and Windows detail helper integration tests."""

from __future__ import annotations

import pathlib
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_webdriver_windows_detail_integration",
        add_module_dir=True,
    )


class WebdriverWindowsDetailIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_webdriver_and_windows_detail_helpers_cover_edges(self) -> None:
        class FakeResponse:
            def __init__(self, payload: bytes) -> None:
                self.payload = payload

            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc, tb):
                return False

            def read(self) -> bytes:
                return self.payload

        self.assertEqual(
            self.mod.webdriver_status_url("http://127.0.0.1:4444/wd/hub?ignored=1"),
            "http://127.0.0.1:4444/wd/hub/status",
        )
        with self.assertRaisesRegex(ValueError, "scheme and host"):
            self.mod.webdriver_status_url("localhost:4444")

        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            side_effect=[
                FakeResponse(b'{"value":{"ready":true,"message":"nested ready"}}'),
                FakeResponse(b'{"ready":false,"message":"top ready"}'),
            ],
        ):
            nested = self.mod.probe_webdriver_endpoint("http://127.0.0.1:4444", timeout=1.0)
            top_level = self.mod.probe_webdriver_endpoint("http://127.0.0.1:4444/status", timeout=1.0)
        self.assertTrue(nested["ready"])
        self.assertEqual(nested["message"], "nested ready")
        self.assertFalse(top_level["ready"])
        self.assertEqual(top_level["message"], "top ready")

        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            side_effect=self.mod.urllib.error.URLError("refused"),
        ):
            with self.assertRaisesRegex(RuntimeError, "refused"):
                self.mod.probe_webdriver_endpoint("http://127.0.0.1:4444")

        self.assertEqual(self.mod.windows_desktop_session_user(None), "")
        self.assertEqual(self.mod.windows_desktop_session_user({"logged_on_user": " dev "}), "dev")
        self.assertEqual(self.mod.windows_desktop_session_state(None), "")
        self.assertEqual(self.mod.windows_desktop_session_state({"session_state": " Active "}), "Active")
        self.assertEqual(self.mod.windows_repo_checkout_detail(None, fallback_path=r"C:\Pulp"), r"C:\Pulp")
        self.assertIn(
            "not a git checkout",
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\Pulp", "repo_exists": True, "git_dir_exists": False}),
        )
        self.assertIn(
            "empty git repo",
            self.mod.windows_repo_checkout_detail({"repo_path": r"C:\Pulp", "git_dir_exists": True, "head_exists": False}),
        )
        self.assertIn(
            "setup.sh missing",
            self.mod.windows_repo_checkout_detail(
                {"repo_path": r"C:\Pulp", "git_dir_exists": True, "head_exists": True, "setup_exists": False}
            ),
        )


if __name__ == "__main__":
    unittest.main()
