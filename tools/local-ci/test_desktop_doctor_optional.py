#!/usr/bin/env python3
"""No-network tests for optional desktop doctor helpers."""

from __future__ import annotations

import io
import pathlib
import unittest
import urllib.error

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("desktop_doctor_optional.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopDoctorOptionalTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_capabilities_merge_optional_features_without_duplicates(self) -> None:
        caps = self.mod.desktop_capabilities_for(
            "macos-local",
            "v3",
            {
                "webview_driver": True,
                "debug_attach": True,
                "video_capture": True,
                "frame_stats": True,
            },
        )

        self.assertEqual(caps.count("debug_attach"), 1)
        self.assertIn("pulp_app_automation", caps)
        self.assertIn("semantic_click", caps)
        self.assertIn("debug_command", caps)
        self.assertIn("video_capture", caps)
        self.assertIn("frame_stats", caps)
        self.assertNotIn("ui_snapshot", self.mod.desktop_capabilities_for("linux-xvfb", "v2"))

    def test_webdriver_status_url_normalizes_status_path(self) -> None:
        self.assertEqual(self.mod.webdriver_status_url("http://driver"), "http://driver/status")
        self.assertEqual(self.mod.webdriver_status_url("http://driver/wd/hub"), "http://driver/wd/hub/status")
        self.assertEqual(self.mod.webdriver_status_url("http://driver/status"), "http://driver/status")
        with self.assertRaisesRegex(ValueError, "scheme and host"):
            self.mod.webdriver_status_url("127.0.0.1:4444")

    def test_probe_webdriver_endpoint_parses_nested_and_top_level_payloads(self) -> None:
        class Response:
            def __init__(self, text: str):
                self._text = text

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def read(self):
                return self._text.encode("utf-8")

        requests = []

        def urlopen(request, *, timeout):
            requests.append((request.full_url, timeout, request.headers.get("Accept")))
            return Response('{"value":{"ready":true,"message":" ok "}}')

        nested = self.mod.probe_webdriver_endpoint("http://driver", timeout=1.5, urlopen_fn=urlopen)

        self.assertEqual(nested["status_url"], "http://driver/status")
        self.assertTrue(nested["ready"])
        self.assertEqual(nested["message"], "ok")
        self.assertEqual(requests, [("http://driver/status", 1.5, "application/json")])

        top_level = self.mod.probe_webdriver_endpoint(
            "http://driver/status",
            urlopen_fn=lambda *_args, **_kwargs: Response('{"ready":false,"message":"down"}'),
        )
        self.assertFalse(top_level["ready"])
        self.assertEqual(top_level["message"], "down")

    def test_probe_webdriver_endpoint_reports_http_url_and_json_errors(self) -> None:
        def http_error(*_args, **_kwargs):
            raise urllib.error.HTTPError("http://driver/status", 500, "boom", {}, io.BytesIO(b"detail"))

        with self.assertRaisesRegex(RuntimeError, "HTTP 500: detail"):
            self.mod.probe_webdriver_endpoint("http://driver", urlopen_fn=http_error)

        def url_error(*_args, **_kwargs):
            raise urllib.error.URLError("refused")

        with self.assertRaisesRegex(RuntimeError, "refused"):
            self.mod.probe_webdriver_endpoint("http://driver", urlopen_fn=url_error)

        class BadJson:
            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def read(self):
                return b"{"

        with self.assertRaisesRegex(RuntimeError, "invalid JSON response"):
            self.mod.probe_webdriver_endpoint("http://driver", urlopen_fn=lambda *_args, **_kwargs: BadJson())


if __name__ == "__main__":
    unittest.main()
