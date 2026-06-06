#!/usr/bin/env python3
"""
Unit tests for fetch_v8_for_release.py.

Covers the per-platform unpack layout, sha256 verification, the
idempotency stamp (skip when the pin matches, re-fetch when it changes),
and the header-only iOS-simulator case (library: false).

Run with:

    python3 -m pytest tools/scripts/test_fetch_v8_for_release.py -v

or without pytest:

    python3 tools/scripts/test_fetch_v8_for_release.py
"""

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

SCRIPT = pathlib.Path(__file__).parent / "fetch_v8_for_release.py"

spec = importlib.util.spec_from_file_location("fetch_v8_for_release", SCRIPT)
fetch_v8 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch_v8)


def _make_zip(zip_path: pathlib.Path, members: dict[str, bytes]) -> str:
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_STORED) as zf:
        for name, data in members.items():
            zf.writestr(name, data)
    h = hashlib.sha256()
    with zip_path.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


@contextlib.contextmanager
def _in_tempdir():
    cwd = pathlib.Path.cwd()
    with tempfile.TemporaryDirectory() as td:
        os.chdir(td)
        try:
            yield pathlib.Path(td)
        finally:
            os.chdir(cwd)


def _write_manifest(repo_root, asset_url, sha, key, *, library=True):
    (repo_root / "tools" / "deps").mkdir(parents=True, exist_ok=True)
    asset = {"url": asset_url, "sha256": sha}
    if not library:
        asset["library"] = False
    manifest = {
        "dependencies": [
            {"name": "V8", "determinism": {"release_assets": {key: asset}}}
        ]
    }
    (repo_root / "tools" / "deps" / "manifest.json").write_text(
        json.dumps(manifest), encoding="utf-8"
    )


class ExpectedLibraryPath(unittest.TestCase):
    def test_mac(self):
        self.assertEqual(
            str(fetch_v8.expected_library_path("mac-arm64")),
            "external/v8-build/mac-arm64/lib/libv8.dylib",
        )

    def test_linux(self):
        self.assertEqual(
            str(fetch_v8.expected_library_path("linux-x64")),
            "external/v8-build/linux-x64/lib/libv8.so",
        )

    def test_windows_uses_import_lib(self):
        # The MSVC linker consumes the import lib; its absence is the real
        # failure that breaks a Windows link, so that's what we check for.
        self.assertEqual(
            str(fetch_v8.expected_library_path("win-x64")),
            "external/v8-build/win-x64/lib/v8.dll.lib",
        )

    def test_android_jnilibs(self):
        self.assertEqual(
            str(fetch_v8.expected_library_path("android-arm64")),
            "external/v8-build/android-arm64/jniLibs/arm64-v8a/libv8.so",
        )

    def test_ios_is_header_only(self):
        self.assertIsNone(fetch_v8.expected_library_path("ios-simulator-arm64"))


class MatrixMap(unittest.TestCase):
    def test_all_release_platforms_wired(self):
        for m in ("darwin-arm64", "darwin-x64", "linux-x64", "linux-arm64",
                  "windows-x64", "windows-arm64", "android-arm64",
                  "ios-simulator-arm64"):
            self.assertIn(m, fetch_v8.MATRIX_TO_MANIFEST)

    def test_darwin_x64_maps_to_mac_x86_64(self):
        self.assertEqual(fetch_v8.MATRIX_TO_MANIFEST["darwin-x64"], "mac-x86_64")


class ArgAndManifestValidation(unittest.TestCase):
    def test_missing_arg(self):
        with _in_tempdir():
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = fetch_v8.main(["fetch_v8_for_release.py"])
        self.assertEqual(rc, 2)
        self.assertIn("usage:", err.getvalue())

    def test_unknown_platform_warns_rc0(self):
        with _in_tempdir():
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = fetch_v8.main(["fetch_v8_for_release.py", "amiga-68k"])
        self.assertEqual(rc, 0)
        self.assertIn("unknown matrix platform", err.getvalue())

    def test_missing_manifest_fails(self):
        with _in_tempdir():
            rc = fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"])
        self.assertEqual(rc, 1)

    def test_manifest_without_v8_fails(self):
        with _in_tempdir() as td:
            (td / "tools" / "deps").mkdir(parents=True)
            (td / "tools" / "deps" / "manifest.json").write_text(
                json.dumps({"dependencies": [{"name": "Other"}]}), encoding="utf-8"
            )
            rc = fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"])
        self.assertEqual(rc, 1)

    def test_known_platform_without_asset_skips(self):
        with _in_tempdir() as td:
            (td / "tools" / "deps").mkdir(parents=True)
            (td / "tools" / "deps" / "manifest.json").write_text(
                json.dumps({"dependencies": [
                    {"name": "V8", "determinism": {"release_assets": {}}}]}),
                encoding="utf-8",
            )
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = fetch_v8.main(["fetch_v8_for_release.py", "windows-arm64"])
        self.assertEqual(rc, 0)
        self.assertIn("manifest key 'win-arm64'", out.getvalue())


class HappyPath(unittest.TestCase):
    def test_mac_arm64_unpacks_and_stamps(self):
        with _in_tempdir() as td:
            zip_path = td / "v8-mac.zip"
            sha = _make_zip(zip_path, {
                "include/v8.h": b"// v8 header",
                "lib/libv8.dylib": b"fake-dylib",
                "manifest.json": b"{}",
            })
            _write_manifest(td, f"file://{zip_path.as_posix()}", sha, "mac-arm64")
            rc = fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"])
            self.assertEqual(rc, 0)
            lib = td / "external/v8-build/mac-arm64/lib/libv8.dylib"
            self.assertTrue(lib.is_file())
            self.assertEqual(lib.read_bytes(), b"fake-dylib")
            stamp = td / "external/v8-build/mac-arm64/.v8-asset-sha256"
            self.assertEqual(stamp.read_text(encoding="utf-8").strip(), sha)
            # Download artifact cleaned up.
            self.assertFalse((td / "v8-release-asset-mac-arm64.zip").exists())

    def test_windows_unpacks_dll_and_implib(self):
        with _in_tempdir() as td:
            zip_path = td / "v8-win.zip"
            sha = _make_zip(zip_path, {
                "include/v8.h": b"// v8 header",
                "lib/v8.dll": b"fake-dll",
                "lib/v8.dll.lib": b"fake-implib",
            })
            _write_manifest(td, f"file://{zip_path.as_posix()}", sha, "win-x64")
            rc = fetch_v8.main(["fetch_v8_for_release.py", "windows-x64"])
            self.assertEqual(rc, 0)
            self.assertTrue((td / "external/v8-build/win-x64/lib/v8.dll").is_file())
            self.assertTrue((td / "external/v8-build/win-x64/lib/v8.dll.lib").is_file())


class IosHeaderOnly(unittest.TestCase):
    def test_ios_simulator_no_library_succeeds(self):
        with _in_tempdir() as td:
            zip_path = td / "v8-ios.zip"
            sha = _make_zip(zip_path, {
                "include/v8.h": b"// v8 header",
                "manifest.json": b"{}",
            })
            _write_manifest(
                td, f"file://{zip_path.as_posix()}", sha,
                "ios-simulator-arm64", library=False,
            )
            rc = fetch_v8.main(["fetch_v8_for_release.py", "ios-simulator-arm64"])
            self.assertEqual(rc, 0, "header-only iOS asset must succeed")
            self.assertTrue((td / "external/v8-build/ios-simulator-arm64/include/v8.h").is_file())
            self.assertTrue((td / "external/v8-build/ios-simulator-arm64/.v8-asset-sha256").is_file())


class MissingLibFails(unittest.TestCase):
    def test_lib_absent_when_required_fails(self):
        with _in_tempdir() as td:
            zip_path = td / "v8-bad.zip"
            sha = _make_zip(zip_path, {"include/v8.h": b"// header, no lib"})
            _write_manifest(td, f"file://{zip_path.as_posix()}", sha, "mac-arm64")
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"])
        self.assertEqual(rc, 1)
        self.assertIn("expected library not found", err.getvalue())


class Sha256Mismatch(unittest.TestCase):
    def test_bad_sha_fails(self):
        with _in_tempdir() as td:
            zip_path = td / "v8.zip"
            _make_zip(zip_path, {"include/v8.h": b"h", "lib/libv8.dylib": b"x"})
            _write_manifest(td, f"file://{zip_path.as_posix()}", "0" * 64, "mac-arm64")
            rc = fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"])
        self.assertEqual(rc, 1)


class IdempotencyStamp(unittest.TestCase):
    def test_second_run_skips_when_stamp_matches(self):
        with _in_tempdir() as td:
            zip_path = td / "v8.zip"
            sha = _make_zip(zip_path, {"include/v8.h": b"h", "lib/libv8.dylib": b"v1"})
            _write_manifest(td, f"file://{zip_path.as_posix()}", sha, "mac-arm64")
            self.assertEqual(fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"]), 0)
            zip_path.unlink()  # a re-download would now fail
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"])
            self.assertEqual(rc, 0)
            self.assertIn("skipping download", out.getvalue())

    def test_pin_change_forces_refetch(self):
        with _in_tempdir() as td:
            z1 = td / "v8-1.zip"
            sha1 = _make_zip(z1, {"include/v8.h": b"h", "lib/libv8.dylib": b"v1"})
            _write_manifest(td, f"file://{z1.as_posix()}", sha1, "mac-arm64")
            self.assertEqual(fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"]), 0)
            lib = td / "external/v8-build/mac-arm64/lib/libv8.dylib"
            self.assertEqual(lib.read_bytes(), b"v1")

            z2 = td / "v8-2.zip"
            sha2 = _make_zip(z2, {"include/v8.h": b"h", "lib/libv8.dylib": b"v2-new"})
            self.assertNotEqual(sha1, sha2)
            _write_manifest(td, f"file://{z2.as_posix()}", sha2, "mac-arm64")
            rc = fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"])
            self.assertEqual(rc, 0)
            self.assertEqual(lib.read_bytes(), b"v2-new",
                             "stale V8 must be replaced when the pin changes")


class BakedV8FastPath(unittest.TestCase):
    """PULP_USE_BAKED_V8 + V8_DIR short-circuit (golden-VM fast path).

    Skips the download only when the baked stamp matches the current pin AND
    the library is present; falls through to a normal fetch otherwise. None of
    these branches were covered before (greptile P1 on this file)."""

    @contextlib.contextmanager
    def _env(self, **kv):
        saved = {k: os.environ.get(k) for k in kv}
        try:
            for k, v in kv.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
            yield
        finally:
            for k, v in saved.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v

    def test_baked_match_skips_download(self):
        with _in_tempdir() as td:
            # Pinned asset whose URL would 404 — a baked match must NOT fetch.
            sha = "ab" * 32
            _write_manifest(td, "file:///definitely/missing.zip", sha, "mac-arm64")
            baked = td / "baked"
            (baked / "mac-arm64" / "lib").mkdir(parents=True)
            (baked / "mac-arm64" / "lib" / "libv8.dylib").write_bytes(b"baked")
            (baked / "mac-arm64" / ".v8-asset-sha256").write_text(sha + "\n")
            out = io.StringIO()
            with self._env(PULP_USE_BAKED_V8="1", V8_DIR=str(baked)), \
                    contextlib.redirect_stdout(out):
                rc = fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"])
            self.assertEqual(rc, 0, "baked match must succeed without fetching")
            self.assertIn("using baked V8", out.getvalue())
            # No download/unpack happened into the checkout.
            self.assertFalse((td / "external/v8-build/mac-arm64/lib/libv8.dylib").exists())

    def test_baked_stale_stamp_falls_through_to_fetch(self):
        with _in_tempdir() as td:
            zip_path = td / "v8.zip"
            real_sha = _make_zip(zip_path, {"include/v8.h": b"h", "lib/libv8.dylib": b"real"})
            _write_manifest(td, f"file://{zip_path.as_posix()}", real_sha, "mac-arm64")
            baked = td / "baked"
            (baked / "mac-arm64" / "lib").mkdir(parents=True)
            (baked / "mac-arm64" / "lib" / "libv8.dylib").write_bytes(b"stale")
            (baked / "mac-arm64" / ".v8-asset-sha256").write_text("00" * 32 + "\n")
            out = io.StringIO()
            with self._env(PULP_USE_BAKED_V8="1", V8_DIR=str(baked)), \
                    contextlib.redirect_stdout(out):
                rc = fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"])
            self.assertEqual(rc, 0)
            self.assertIn("re-fetching", out.getvalue())
            # Stale stamp → real asset fetched into the checkout.
            lib = td / "external/v8-build/mac-arm64/lib/libv8.dylib"
            self.assertTrue(lib.is_file())
            self.assertEqual(lib.read_bytes(), b"real")


if __name__ == "__main__":
    _real_stdout = sys.stdout
    sys.stdout = io.StringIO()
    try:
        result = unittest.main(verbosity=2, exit=False).result
    finally:
        sys.stdout = _real_stdout
    print(
        f"\nfetch_v8_for_release tests: ran {result.testsRun}, "
        f"failures={len(result.failures)}, errors={len(result.errors)}"
    )
    sys.exit(0 if result.wasSuccessful() else 1)
