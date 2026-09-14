#!/usr/bin/env python3
"""Additional unit tests for tools/scripts/fetch_skia_for_release.py."""

from __future__ import annotations

import contextlib
import email.message
import hashlib
import http.client
import importlib.util
import io
import json
import os
import pathlib
import runpy
import sys
import tempfile
import unittest
import urllib.error
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


def make_provider_zip_bytes(payload: bytes = b"skia") -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        zf.writestr("build/mac-gpu/lib/Release/libskia.a", payload)
        zf.writestr("build/mac-gpu/lib/Release/libdawn_combined.a", b"dawn-" + payload)
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

    def test_script_entrypoint_returns_usage_error(self) -> None:
        with mock.patch.object(sys, "argv", [str(SCRIPT)]):
            with self.assertRaises(SystemExit) as cm:
                runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(cm.exception.code, 2)

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
            data = make_provider_zip_bytes(b"abc")
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
            self.assertIn("expected materialized Skia/Dawn archives not found", err.getvalue())


class BakedSkiaShortCircuit(unittest.TestCase):
    """PULP_USE_BAKED_SKIA: the Tart VM runner bakes Skia at $SKIA_DIR, so the
    workflow fetch is redundant — but only skip when the baked stamp matches the
    current pin, so a pin bump on a stale golden re-fetches (never stuck)."""

    def _manifest(self, digest: str) -> None:
        manifest = pathlib.Path("tools/deps/manifest.json")
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(
            json.dumps({"dependencies": [{
                "name": "Skia",
                "determinism": {"release_assets": {
                    "mac-arm64": {"url": "https://example.invalid/skia.zip", "sha256": digest},
                }},
            }]}),
            encoding="utf-8",
        )

    def _bake(self, root: pathlib.Path, data: bytes, stamp: str) -> None:
        root.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            archive.extractall(root)
        (root / skia.SOURCE_ARCHIVE).write_bytes(data)
        skia.write_generation_receipt(root, "darwin-arm64", stamp)
        (root / ".skia-asset-sha256").write_text(stamp, encoding="utf-8")

    def test_baked_matching_stamp_skips_fetch_without_download(self) -> None:
        with tempfile.TemporaryDirectory() as td, cwd(pathlib.Path(td)):
            data = make_provider_zip_bytes(b"baked")
            digest = hashlib.sha256(data).hexdigest()
            self._manifest(digest)
            baked = pathlib.Path("baked"); self._bake(baked, data, digest)

            def _boom(*a, **k):
                raise AssertionError("urlopen must NOT be called when baked Skia matches the pin")

            out = io.StringIO()
            with mock.patch.dict(os.environ, {"PULP_USE_BAKED_SKIA": "1", "SKIA_DIR": str(baked.resolve())}), \
                 mock.patch.object(skia.urllib.request, "urlopen", side_effect=_boom), \
                 contextlib.redirect_stdout(out):
                self.assertEqual(skia.main(["fetch", "darwin-arm64"]), 0)
            self.assertIn("PULP_USE_BAKED_SKIA", out.getvalue())
            self.assertIn("skipping fetch", out.getvalue())
            self.assertFalse(pathlib.Path("external/skia-build").exists())

    def test_baked_stale_stamp_refetches(self) -> None:
        with tempfile.TemporaryDirectory() as td, cwd(pathlib.Path(td)):
            data = make_provider_zip_bytes(b"fresh")
            digest = hashlib.sha256(data).hexdigest()
            self._manifest(digest)
            old_data = make_provider_zip_bytes(b"stale")
            old_digest = hashlib.sha256(old_data).hexdigest()
            baked = pathlib.Path("baked"); self._bake(baked, old_data, old_digest)

            out = io.StringIO()
            with mock.patch.dict(os.environ, {"PULP_USE_BAKED_SKIA": "1", "SKIA_DIR": str(baked.resolve())}), \
                 mock.patch.object(skia.urllib.request, "urlopen", return_value=_FakeResponse(data)), \
                 contextlib.redirect_stdout(out):
                self.assertEqual(skia.main(["fetch", "darwin-arm64"]), 0)
            self.assertIn("missing or stale", out.getvalue())
            self.assertTrue(pathlib.Path("external/skia-build/build/mac-gpu/lib/Release/libskia.a").is_file())


    def test_main_unpacks_over_a_prepopulated_destination(self) -> None:
        # A prior cache restore can leave external/skia-build/build already present.
        # Unpacking must overwrite it, not raise FileExistsError on the dir entry.
        with tempfile.TemporaryDirectory() as td, cwd(pathlib.Path(td)):
            data = make_provider_zip_bytes(b"fresh")
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
            # Pre-populate the destination tree with stale bytes (the trigger).
            stale = pathlib.Path("external/skia-build/build/mac-gpu/lib/Release")
            stale.mkdir(parents=True)
            (stale / "libskia.a").write_text("stale")

            with mock.patch.object(skia.urllib.request, "urlopen", return_value=_FakeResponse(data)), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(skia.main(["fetch", "darwin-arm64"]), 0)

            # Overwritten with the fresh bytes; no exception on the existing dir.
            self.assertEqual(skia.expected_library_path("darwin-arm64").read_bytes(), b"fresh")

    def test_main_self_heals_a_dangling_build_symlink(self) -> None:
        # A warm runner can leave external/skia-build/build as a dangling symlink
        # (its target gone). Unpacking must remove the broken link and succeed,
        # not abort with the FileNotFound/FileExists pair the link produces.
        with tempfile.TemporaryDirectory() as td, cwd(pathlib.Path(td)):
            data = make_provider_zip_bytes(b"ok")
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
            # external/skia-build/build → a target that does not exist (dangling).
            pathlib.Path("external/skia-build").mkdir(parents=True)
            pathlib.Path("external/skia-build/build").symlink_to("nonexistent-target")
            self.assertTrue(pathlib.Path("external/skia-build/build").is_symlink())
            self.assertFalse(pathlib.Path("external/skia-build/build").exists())  # dangling

            with mock.patch.object(skia.urllib.request, "urlopen", return_value=_FakeResponse(data)), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(skia.main(["fetch", "darwin-arm64"]), 0)

            # The broken link is gone, replaced by a real tree with the fetched lib.
            self.assertFalse(pathlib.Path("external/skia-build/build").is_symlink())
            self.assertEqual(skia.expected_library_path("darwin-arm64").read_bytes(), b"ok")


def _http_error(code: int, retry_after: str | None = None) -> urllib.error.HTTPError:
    headers = email.message.Message()
    if retry_after is not None:
        headers["Retry-After"] = retry_after
    return urllib.error.HTTPError(
        "https://example.invalid/skia.zip", code, "boom", headers, None
    )


class _TruncatedResponse:
    """Yields one chunk, then fails the way a dropped connection does."""

    def __init__(self, prefix: bytes):
        self._prefix = prefix
        self._sent = False

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self, size: int = -1) -> bytes:
        if self._sent:
            raise http.client.IncompleteRead(b"", 4096)
        self._sent = True
        return self._prefix


class DownloadRetry(unittest.TestCase):
    """The pinned Skia asset is fetched once per macOS gate run; a single
    transient 5xx there ejects a PR from the merge queue."""

    def _zip_path(self, td: str) -> pathlib.Path:
        return pathlib.Path(td) / "asset.zip"

    def test_transient_5xx_is_retried_and_succeeds(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            dest = self._zip_path(td)
            responses = [_http_error(502), _FakeResponse(b"payload")]
            slept: list[float] = []
            with mock.patch.object(skia.urllib.request, "urlopen",
                                   side_effect=responses) as urlopen, \
                 contextlib.redirect_stdout(io.StringIO()):
                skia.download_release_asset(
                    "https://example.invalid/skia.zip", dest, sleep=slept.append
                )
            self.assertEqual(dest.read_bytes(), b"payload")
            self.assertEqual(urlopen.call_count, 2)
            self.assertEqual(len(slept), 1, "exactly one backoff between two attempts")

    def test_permanent_404_is_not_retried(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            slept: list[float] = []
            with mock.patch.object(skia.urllib.request, "urlopen",
                                   side_effect=_http_error(404)) as urlopen, \
                 contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(urllib.error.HTTPError) as cm:
                    skia.download_release_asset(
                        "https://example.invalid/skia.zip", self._zip_path(td),
                        sleep=slept.append,
                    )
            self.assertEqual(cm.exception.code, 404)
            self.assertEqual(urlopen.call_count, 1, "a bad pin must fail immediately")
            self.assertEqual(slept, [])

    def test_exhausting_attempts_raises_the_last_error(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            slept: list[float] = []
            err = urllib.error.URLError("connection reset")
            with mock.patch.object(skia.urllib.request, "urlopen",
                                   side_effect=err) as urlopen, \
                 contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(urllib.error.URLError, "connection reset"):
                    skia.download_release_asset(
                        "https://example.invalid/skia.zip", self._zip_path(td),
                        attempts=3, sleep=slept.append,
                    )
            self.assertEqual(urlopen.call_count, 3)
            self.assertEqual(len(slept), 2, "no backoff after the final attempt")

    def test_backoff_grows_and_honours_retry_after(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            slept: list[float] = []
            with mock.patch.object(skia.urllib.request, "urlopen",
                                   side_effect=urllib.error.URLError("down")), \
                 contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(urllib.error.URLError):
                    skia.download_release_asset(
                        "https://example.invalid/skia.zip", self._zip_path(td),
                        attempts=3, sleep=slept.append,
                    )
            self.assertEqual(slept, [2.0, 4.0])

            served: list[float] = []
            with mock.patch.object(skia.urllib.request, "urlopen",
                                   side_effect=[_http_error(503, "7"),
                                                _FakeResponse(b"ok")]), \
                 contextlib.redirect_stdout(io.StringIO()):
                skia.download_release_asset(
                    "https://example.invalid/skia.zip", self._zip_path(td),
                    sleep=served.append,
                )
            self.assertEqual(served, [7.0], "a numeric Retry-After overrides the schedule")

    def test_partial_body_is_truncated_not_appended(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            dest = self._zip_path(td)
            with mock.patch.object(skia.urllib.request, "urlopen",
                                   side_effect=[_TruncatedResponse(b"HALF"),
                                                _FakeResponse(b"WHOLE")]), \
                 contextlib.redirect_stdout(io.StringIO()):
                skia.download_release_asset(
                    "https://example.invalid/skia.zip", dest, sleep=lambda _s: None
                )
            self.assertEqual(dest.read_bytes(), b"WHOLE",
                             "a retry must not concatenate onto the failed body")


class DownloadRetryThroughMain(unittest.TestCase):
    def test_fetch_survives_a_transient_502(self) -> None:
        with tempfile.TemporaryDirectory() as td, cwd(pathlib.Path(td)):
            data = make_provider_zip_bytes(b"retried")
            digest = hashlib.sha256(data).hexdigest()
            pathlib.Path("tools/deps").mkdir(parents=True)
            pathlib.Path("tools/deps/manifest.json").write_text(
                json.dumps({"dependencies": [{
                    "name": "Skia",
                    "determinism": {"release_assets": {
                        "mac-arm64": {
                            "url": "https://example.invalid/skia.zip",
                            "sha256": digest,
                        },
                    }},
                }]}),
                encoding="utf-8",
            )
            with mock.patch.object(skia.urllib.request, "urlopen",
                                   side_effect=[_http_error(502),
                                                _FakeResponse(data)]) as urlopen, \
                 mock.patch.object(skia.time, "sleep"), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(skia.main(["fetch", "darwin-arm64"]), 0)
            self.assertEqual(urlopen.call_count, 2)
            self.assertEqual(
                skia.expected_library_path("darwin-arm64").read_bytes(), b"retried"
            )


if __name__ == "__main__":
    unittest.main()
