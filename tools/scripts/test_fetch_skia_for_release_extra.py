#!/usr/bin/env python3
"""Additional unit tests for tools/scripts/fetch_skia_for_release.py."""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import sys
import tempfile
import unittest
import zipfile
from unittest import mock


SCRIPT = pathlib.Path(__file__).parent / "fetch_skia_for_release.py"

spec = importlib.util.spec_from_file_location("fetch_skia_for_release_extra_target", SCRIPT)
assert spec and spec.loader
skia = importlib.util.module_from_spec(spec)
sys.modules["fetch_skia_for_release_extra_target"] = skia
spec.loader.exec_module(skia)


class _FakeResponse:
    def __init__(self, data: bytes):
        self._data = data
        self._offset = 0

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self, size: int = -1) -> bytes:
        if self._offset >= len(self._data):
            return b""
        if size < 0:
            size = len(self._data) - self._offset
        chunk = self._data[self._offset:self._offset + size]
        self._offset += len(chunk)
        return chunk


def make_zip_bytes(rel_path: pathlib.Path, payload: bytes = b"skia") -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr(rel_path.as_posix(), payload)
    return buf.getvalue()


@contextlib.contextmanager
def cwd(path: pathlib.Path):
    old = pathlib.Path.cwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(old)


class FetchSkiaForReleaseExtraTests(unittest.TestCase):
    def test_main_usage_unknown_platform_and_missing_manifest_paths(self) -> None:
        with tempfile.TemporaryDirectory() as td, cwd(pathlib.Path(td)):
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(skia.main(["fetch"]), 2)
            self.assertIn("usage:", err.getvalue())

            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(skia.main(["fetch", "weird-platform"]), 0)
            self.assertIn("unknown matrix platform", err.getvalue())

            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(skia.main(["fetch", "darwin-arm64"]), 1)
            self.assertIn("manifest.json not found", err.getvalue())

    def test_main_skips_platform_without_release_asset(self) -> None:
        with tempfile.TemporaryDirectory() as td, cwd(pathlib.Path(td)):
            manifest = pathlib.Path("tools/deps/manifest.json")
            manifest.parent.mkdir(parents=True)
            manifest.write_text(
                json.dumps({"dependencies": [{"name": "Skia", "determinism": {"release_assets": {}}}]}),
                encoding="utf-8",
            )

            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(skia.main(["fetch", "linux-arm64"]), 0)
            self.assertIn("no Skia release asset", out.getvalue())

    def test_main_reports_missing_skia_entry_and_checksum_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as td, cwd(pathlib.Path(td)):
            manifest = pathlib.Path("tools/deps/manifest.json")
            manifest.parent.mkdir(parents=True)
            manifest.write_text(json.dumps({"dependencies": []}), encoding="utf-8")
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(skia.main(["fetch", "darwin-arm64"]), 1)
            self.assertIn("no 'Skia' dependency", err.getvalue())

            manifest.write_text(
                json.dumps({
                    "dependencies": [{
                        "name": "Skia",
                        "determinism": {
                            "release_assets": {
                                "mac-arm64": {"url": "https://example.invalid/skia.zip", "sha256": "0" * 64}
                            }
                        },
                    }]
                }),
                encoding="utf-8",
            )
            with mock.patch.object(skia.urllib.request, "urlopen", return_value=_FakeResponse(b"not a zip")), \
                 contextlib.redirect_stderr(err := io.StringIO()), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(skia.main(["fetch", "darwin-arm64"]), 1)
            self.assertIn("sha256 mismatch", err.getvalue())

    def test_main_unpacks_valid_asset_and_reports_zip_layout_drift(self) -> None:
        with tempfile.TemporaryDirectory() as td, cwd(pathlib.Path(td)):
            data = make_zip_bytes(pathlib.Path("build/mac-gpu/lib/Release/libskia.a"), b"abc")
            digest = hashlib.sha256(data).hexdigest()
            manifest = pathlib.Path("tools/deps/manifest.json")
            manifest.parent.mkdir(parents=True)
            manifest.write_text(
                json.dumps({
                    "dependencies": [{
                        "name": "skia",
                        "determinism": {
                            "release_assets": {
                                "mac-arm64": {"url": "https://example.invalid/skia.zip", "sha256": digest}
                            }
                        },
                    }]
                }),
                encoding="utf-8",
            )

            out = io.StringIO()
            with mock.patch.object(skia.urllib.request, "urlopen", return_value=_FakeResponse(data)), \
                 contextlib.redirect_stdout(out):
                self.assertEqual(skia.main(["fetch", "darwin-arm64"]), 0)

            self.assertTrue(skia.expected_library_path("darwin-arm64").is_file())
            self.assertFalse(pathlib.Path("skia-release-asset.zip").exists())
            self.assertIn("OK:", out.getvalue())

        with tempfile.TemporaryDirectory() as td, cwd(pathlib.Path(td)):
            data = make_zip_bytes(pathlib.Path("wrong/place/libskia.a"), b"abc")
            digest = hashlib.sha256(data).hexdigest()
            manifest = pathlib.Path("tools/deps/manifest.json")
            manifest.parent.mkdir(parents=True)
            manifest.write_text(
                json.dumps({
                    "dependencies": [{
                        "name": "Skia",
                        "determinism": {
                            "release_assets": {
                                "mac-arm64": {"url": "https://example.invalid/skia.zip", "sha256": digest}
                            }
                        },
                    }]
                }),
                encoding="utf-8",
            )

            with mock.patch.object(skia.urllib.request, "urlopen", return_value=_FakeResponse(data)), \
                 contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(err := io.StringIO()):
                self.assertEqual(skia.main(["fetch", "darwin-arm64"]), 1)
            self.assertIn("expected library not found", err.getvalue())


if __name__ == "__main__":
    unittest.main()
