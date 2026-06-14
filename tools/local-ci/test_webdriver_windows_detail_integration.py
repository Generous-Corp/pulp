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

    def test_webdriver_probe_parses_status_shapes_and_errors(self) -> None:
        self.assertEqual(self.mod.webdriver_status_url("http://127.0.0.1:4444"), "http://127.0.0.1:4444/status")
        self.assertEqual(self.mod.webdriver_status_url("http://host/wd/hub"), "http://host/wd/hub/status")
        self.assertEqual(self.mod.webdriver_status_url("http://host/status?old=1#frag"), "http://host/status")
        with self.assertRaisesRegex(ValueError, "scheme and host"):
            self.mod.webdriver_status_url("localhost:4444")

        class FakeResponse:
            def __init__(self, payload: str) -> None:
                self.payload = payload

            def __enter__(self):
                return self

            def __exit__(self, *_exc):
                return False

            def read(self) -> bytes:
                return self.payload.encode("utf-8")

        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            return_value=FakeResponse('{"value":{"ready":true,"message":" ok "}}'),
        ) as urlopen:
            probe = self.mod.probe_webdriver_endpoint("http://driver")
            self.assertEqual(probe["status_url"], "http://driver/status")
            self.assertTrue(probe["ready"])
            self.assertEqual(probe["message"], "ok")
            self.assertEqual(urlopen.call_args.kwargs["timeout"], 5.0)

        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            return_value=FakeResponse('{"ready":false,"message":"not ready"}'),
        ):
            probe = self.mod.probe_webdriver_endpoint("http://driver/status", timeout=1.5)
            self.assertFalse(probe["ready"])
            self.assertEqual(probe["message"], "not ready")

        http_error = self.mod.urllib.error.HTTPError(
            "http://driver/status",
            500,
            "boom",
            {},
            mock.Mock(read=lambda: b"server body"),
        )
        with mock.patch.object(self.mod.urllib.request, "urlopen", side_effect=http_error):
            with self.assertRaisesRegex(RuntimeError, "HTTP 500: server body"):
                self.mod.probe_webdriver_endpoint("http://driver")
        with mock.patch.object(self.mod.urllib.request, "urlopen", return_value=FakeResponse("{bad")):
            with self.assertRaisesRegex(RuntimeError, "invalid JSON response"):
                self.mod.probe_webdriver_endpoint("http://driver")
        with mock.patch.object(
            self.mod.urllib.request,
            "urlopen",
            side_effect=self.mod.urllib.error.URLError("connection refused"),
        ):
            with self.assertRaisesRegex(RuntimeError, "connection refused"):
                self.mod.probe_webdriver_endpoint("http://driver")
        with mock.patch.object(self.mod.urllib.request, "urlopen", return_value=FakeResponse("[]")):
            probe = self.mod.probe_webdriver_endpoint("http://driver")
        self.assertIsNone(probe["ready"])
        self.assertEqual(probe["message"], "")
        self.assertEqual(probe["payload"], [])


if __name__ == "__main__":
    unittest.main()
