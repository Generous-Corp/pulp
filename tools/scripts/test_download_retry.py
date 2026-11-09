#!/usr/bin/env python3
"""Tests for the shared CI download retry policy (download_retry.py)."""

from __future__ import annotations

import contextlib
import http.server
import io
import pathlib
import subprocess
import sys
import tempfile
import threading
import unittest
import urllib.error
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import download_retry  # noqa: E402

SCRIPT = pathlib.Path(download_retry.__file__)


class _Server:
    """Local HTTP server replaying a scripted list of status codes per path."""

    def __init__(self, statuses, body=b"payload"):
        self.statuses = list(statuses)
        self.body = body
        self.hits = 0
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802
                outer.hits += 1
                status = outer.statuses.pop(0) if outer.statuses else 200
                self.send_response(status)
                self.send_header("Content-Length", str(len(outer.body) if status == 200 else 0))
                self.end_headers()
                if status == 200:
                    self.wfile.write(outer.body)

            def log_message(self, *args):
                pass

        self.httpd = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_port}/asset"
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.httpd.shutdown()
        self.httpd.server_close()


class DownloadRetryPolicy(unittest.TestCase):
    def _run(self, statuses, **kwargs):
        slept: list[float] = []
        with tempfile.TemporaryDirectory() as td, _Server(statuses) as srv:
            dest = pathlib.Path(td) / "out"
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    download_retry.download(srv.url, dest, sleep=slept.append, **kwargs)
                data = dest.read_bytes()
            except urllib.error.HTTPError as err:
                return srv.hits, slept, err
            return srv.hits, slept, data

    def test_504_twice_then_200_succeeds(self):
        hits, slept, data = self._run([504, 504, 200])
        self.assertEqual(data, b"payload")
        self.assertEqual(hits, 3)
        self.assertEqual(len(slept), 2)

    def test_404_fails_immediately(self):
        hits, slept, err = self._run([404])
        self.assertIsInstance(err, urllib.error.HTTPError)
        self.assertEqual(err.code, 404)
        self.assertEqual((hits, slept), (1, []))

    def test_403_fails_immediately(self):
        hits, slept, err = self._run([403])
        self.assertEqual(err.code, 403)
        self.assertEqual(hits, 1)

    def test_default_policy_is_six_attempts(self):
        hits, slept, err = self._run([503] * 10)
        self.assertEqual(err.code, 503)
        self.assertEqual(hits, 6)
        self.assertEqual(len(slept), 5, "no backoff after the final attempt")

    def test_total_nominal_wait_is_two_to_three_minutes(self):
        with mock.patch.object(download_retry, "jitter_factor", return_value=1.0):
            delays = [download_retry.backoff_seconds(i) for i in range(1, 6)]
        self.assertEqual(delays, [5.0, 10.0, 20.0, 40.0, 60.0])
        self.assertTrue(120 <= sum(delays) <= 180)

    def test_jitter_stays_within_bounds(self):
        for _ in range(200):
            d = download_retry.backoff_seconds(3)
            self.assertTrue(20.0 * 0.75 <= d <= 20.0 * 1.25)

    def test_connection_reset_is_retried(self):
        slept: list[float] = []
        with tempfile.TemporaryDirectory() as td, \
                mock.patch.object(download_retry.urllib.request, "urlopen",
                                  side_effect=[ConnectionResetError("reset"), ConnectionResetError("reset")]) as op, \
                contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(ConnectionResetError):
                download_retry.download("http://x.invalid/a", pathlib.Path(td) / "o",
                                        attempts=2, sleep=slept.append)
        self.assertEqual(op.call_count, 2)

    def test_cli_exit_codes_and_part_file(self):
        with tempfile.TemporaryDirectory() as td, _Server([503, 200]) as srv:
            dest = pathlib.Path(td) / "out.bin"
            # The CLI sleeps for real; one retry costs ~5s at most 6.25s.
            ok = subprocess.run([sys.executable, str(SCRIPT), srv.url, str(dest)],
                                capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(ok.returncode, 0, ok.stderr)
            self.assertEqual(dest.read_bytes(), b"payload")
            self.assertFalse(dest.with_name("out.bin.part").exists())
        with tempfile.TemporaryDirectory() as td, _Server([404]) as srv:
            dest = pathlib.Path(td) / "out.bin"
            bad = subprocess.run([sys.executable, str(SCRIPT), srv.url, str(dest)],
                                 capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(bad.returncode, 1)
            self.assertFalse(dest.exists())
            self.assertEqual(srv.hits, 1)


if __name__ == "__main__":
    unittest.main()
