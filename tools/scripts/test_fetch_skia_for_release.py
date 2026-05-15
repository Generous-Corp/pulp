#!/usr/bin/env python3
"""
Unit tests for fetch_skia_for_release.py.

Covers the chrome/m144 arch-subdir layout regression (pulp #1962) that
left every SDK release after v0.94.0 unpublished because the script's
existence check expected `Release/libskia.a` but the upstream zips
shipped `Release/<arch>/libskia.a`.

Run with:

    python3 -m pytest tools/scripts/test_fetch_skia_for_release.py -v

or without pytest:

    python3 tools/scripts/test_fetch_skia_for_release.py
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

SCRIPT = pathlib.Path(__file__).parent / "fetch_skia_for_release.py"

spec = importlib.util.spec_from_file_location("fetch_skia_for_release", SCRIPT)
fetch_skia = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch_skia)


def _make_zip(zip_path: pathlib.Path, members: dict[str, bytes]) -> str:
    """Build a zip with the given member→bytes mapping and return its sha256."""
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


def _write_manifest(repo_root: pathlib.Path, asset_url: str, sha: str, key: str) -> None:
    (repo_root / "tools" / "deps").mkdir(parents=True, exist_ok=True)
    manifest = {
        "dependencies": [
            {
                "name": "Skia",
                "determinism": {
                    "release_assets": {
                        key: {"url": asset_url, "sha256": sha},
                    },
                },
            }
        ]
    }
    (repo_root / "tools" / "deps" / "manifest.json").write_text(
        json.dumps(manifest), encoding="utf-8"
    )


class ExpectedLibraryPath(unittest.TestCase):
    def test_darwin_arm64(self):
        p = fetch_skia.expected_library_path("darwin-arm64")
        self.assertEqual(
            str(p), "external/skia-build/build/mac-gpu/lib/Release/libskia.a"
        )

    def test_linux_x64(self):
        p = fetch_skia.expected_library_path("linux-x64")
        self.assertEqual(
            str(p), "external/skia-build/build/linux-gpu/lib/Release/libskia.a"
        )

    def test_windows_x64(self):
        p = fetch_skia.expected_library_path("windows-x64")
        self.assertEqual(
            str(p), "external/skia-build/build/win-gpu/lib/Release/skia.lib"
        )

    def test_unknown(self):
        with self.assertRaises(SystemExit):
            fetch_skia.expected_library_path("haiku-ppc")


class UnknownMatrixPlatform(unittest.TestCase):
    def test_missing_platform_argument_returns_usage_error(self):
        with _in_tempdir():
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = fetch_skia.main(["fetch_skia_for_release.py"])

        self.assertEqual(rc, 2)
        self.assertIn("usage:", err.getvalue())

    def test_returns_zero_with_warning(self):
        with _in_tempdir():
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = fetch_skia.main(
                    ["fetch_skia_for_release.py", "amiga-68k"]
                )

        self.assertEqual(rc, 0)
        self.assertIn("unknown matrix platform", err.getvalue())


class ManifestValidation(unittest.TestCase):
    def test_missing_manifest_fails_for_known_platform(self):
        with _in_tempdir():
            rc = fetch_skia.main(
                ["fetch_skia_for_release.py", "darwin-arm64"]
            )
        self.assertEqual(rc, 1)

    def test_manifest_without_skia_dependency_fails(self):
        with _in_tempdir() as td:
            (td / "tools" / "deps").mkdir(parents=True)
            (td / "tools" / "deps" / "manifest.json").write_text(
                json.dumps({"dependencies": [{"name": "Other"}]}),
                encoding="utf-8",
            )

            rc = fetch_skia.main(
                ["fetch_skia_for_release.py", "darwin-arm64"]
            )

        self.assertEqual(rc, 1)

    def test_known_platform_without_asset_skips(self):
        with _in_tempdir() as td:
            (td / "tools" / "deps").mkdir(parents=True)
            manifest = {
                "dependencies": [
                    {
                        "name": "Skia",
                        "determinism": {"release_assets": {}},
                    }
                ]
            }
            (td / "tools" / "deps" / "manifest.json").write_text(
                json.dumps(manifest), encoding="utf-8"
            )

            rc = fetch_skia.main(
                ["fetch_skia_for_release.py", "windows-arm64"]
            )

        self.assertEqual(rc, 0)


class FlatLayoutSucceeds(unittest.TestCase):
    """Pre-m144 layout: libs flat under Release/ (no arch subdir)."""

    def test_darwin_arm64_flat(self):
        with _in_tempdir() as td:
            zip_path = td / "skia-mac.zip"
            payload = {
                "build/mac-gpu/lib/Release/libskia.a": b"skia-flat",
                "build/include/include/core/SkCanvas.h": b"// header",
            }
            sha = _make_zip(zip_path, payload)
            _write_manifest(
                td, f"file://{zip_path.as_posix()}", sha, "mac-arm64"
            )
            rc = fetch_skia.main(
                ["fetch_skia_for_release.py", "darwin-arm64"]
            )
            self.assertEqual(rc, 0)
            expected = (
                td
                / "external/skia-build/build/mac-gpu/lib/Release/libskia.a"
            )
            self.assertTrue(expected.is_file())
            self.assertEqual(expected.read_bytes(), b"skia-flat")


class ArchSubdirLayoutFlattens(unittest.TestCase):
    """chrome/m144 layout: libs under Release/<arch>/. Must flatten."""

    def test_darwin_arm64_arch_subdir(self):
        with _in_tempdir() as td:
            zip_path = td / "skia-mac.zip"
            payload = {
                # Arch-subdir layout — what skia-builder chrome/m144 ships.
                "build/mac-gpu/lib/Release/arm64/libskia.a": b"skia-arch",
                "build/mac-gpu/lib/Release/arm64/libdawn_combined.a": b"dawn-arch",
                "build/mac-gpu/lib/Release/arm64/libskparagraph.a": b"para-arch",
            }
            sha = _make_zip(zip_path, payload)
            _write_manifest(
                td, f"file://{zip_path.as_posix()}", sha, "mac-arm64"
            )
            rc = fetch_skia.main(
                ["fetch_skia_for_release.py", "darwin-arm64"]
            )
            self.assertEqual(rc, 0, "fetch must succeed on arch-subdir layout")
            release_dir = (
                td / "external/skia-build/build/mac-gpu/lib/Release"
            )
            # Libs were flattened up from arm64/.
            for lib in ("libskia.a", "libdawn_combined.a", "libskparagraph.a"):
                self.assertTrue(
                    (release_dir / lib).is_file(),
                    f"{lib} should be flattened into Release/",
                )
            # arm64/ subdir was removed once emptied.
            self.assertFalse(
                (release_dir / "arm64").exists(),
                "arm64/ subdir should be removed after flatten",
            )

    def test_arch_subdir_with_nested_dir_keeps_nonempty_dir(self):
        with _in_tempdir() as td:
            zip_path = td / "skia-mac.zip"
            payload = {
                "build/mac-gpu/lib/Release/arm64/libskia.a": b"skia-arch",
                "build/mac-gpu/lib/Release/arm64/obj/keep.txt": b"keep",
            }
            sha = _make_zip(zip_path, payload)
            _write_manifest(
                td, f"file://{zip_path.as_posix()}", sha, "mac-arm64"
            )
            rc = fetch_skia.main(
                ["fetch_skia_for_release.py", "darwin-arm64"]
            )

            self.assertEqual(rc, 0)
            release_dir = (
                td / "external/skia-build/build/mac-gpu/lib/Release"
            )
            self.assertTrue((release_dir / "libskia.a").is_file())
            self.assertTrue((release_dir / "arm64" / "obj" / "keep.txt").is_file())

    def test_linux_x64_arch_subdir(self):
        with _in_tempdir() as td:
            zip_path = td / "skia-linux.zip"
            payload = {
                "build/linux-gpu/lib/Release/x64/libskia.a": b"linux-skia",
                "build/linux-gpu/lib/Release/x64/libdawn_combined.a": b"linux-dawn",
            }
            sha = _make_zip(zip_path, payload)
            _write_manifest(
                td, f"file://{zip_path.as_posix()}", sha, "linux-x64"
            )
            rc = fetch_skia.main(["fetch_skia_for_release.py", "linux-x64"])
            self.assertEqual(rc, 0)
            expected = (
                td
                / "external/skia-build/build/linux-gpu/lib/Release/libskia.a"
            )
            self.assertTrue(expected.is_file())
            self.assertEqual(expected.read_bytes(), b"linux-skia")

    def test_windows_x64_arch_subdir(self):
        with _in_tempdir() as td:
            zip_path = td / "skia-win.zip"
            payload = {
                "build/win-gpu/lib/Release/x64/skia.lib": b"windows-skia",
                "build/win-gpu/lib/Release/x64/skparagraph.lib": b"windows-para",
            }
            sha = _make_zip(zip_path, payload)
            _write_manifest(
                td, f"file://{zip_path.as_posix()}", sha, "win-x64"
            )

            rc = fetch_skia.main(["fetch_skia_for_release.py", "windows-x64"])

            self.assertEqual(rc, 0)
            release_dir = (
                td / "external/skia-build/build/win-gpu/lib/Release"
            )
            self.assertEqual((release_dir / "skia.lib").read_bytes(), b"windows-skia")
            self.assertEqual(
                (release_dir / "skparagraph.lib").read_bytes(), b"windows-para"
            )

    def test_arch_subdir_does_not_clobber_existing_flat_file(self):
        with _in_tempdir() as td:
            zip_path = td / "skia-mac.zip"
            payload = {
                "build/mac-gpu/lib/Release/arm64/libskia.a": b"arch-copy",
                "build/mac-gpu/lib/Release/libdawn_combined.a": b"already-flat",
                "build/mac-gpu/lib/Release/arm64/libdawn_combined.a": b"dawn",
            }
            sha = _make_zip(zip_path, payload)
            _write_manifest(
                td, f"file://{zip_path.as_posix()}", sha, "mac-arm64"
            )

            rc = fetch_skia.main(
                ["fetch_skia_for_release.py", "darwin-arm64"]
            )

            self.assertEqual(rc, 0)
            release_dir = (
                td / "external/skia-build/build/mac-gpu/lib/Release"
            )
            self.assertEqual((release_dir / "libskia.a").read_bytes(), b"arch-copy")
            self.assertEqual(
                (release_dir / "libdawn_combined.a").read_bytes(), b"already-flat"
            )
            self.assertTrue((release_dir / "arm64" / "libdawn_combined.a").is_file())


class MissingLibFails(unittest.TestCase):
    """Zip without libs anywhere must still surface a clear error."""

    def test_no_lib_anywhere(self):
        with _in_tempdir() as td:
            zip_path = td / "skia-empty.zip"
            payload = {
                "build/mac-gpu/lib/Release/README.txt": b"no libs here",
            }
            sha = _make_zip(zip_path, payload)
            _write_manifest(
                td, f"file://{zip_path.as_posix()}", sha, "mac-arm64"
            )
            rc = fetch_skia.main(
                ["fetch_skia_for_release.py", "darwin-arm64"]
            )
            self.assertEqual(rc, 1, "missing libskia.a must exit non-zero")


class Sha256MismatchFails(unittest.TestCase):
    def test_bad_sha(self):
        with _in_tempdir() as td:
            zip_path = td / "skia-mac.zip"
            payload = {
                "build/mac-gpu/lib/Release/arm64/libskia.a": b"skia-arch",
            }
            _ = _make_zip(zip_path, payload)
            _write_manifest(
                td,
                f"file://{zip_path.as_posix()}",
                "0" * 64,  # deliberately wrong
                "mac-arm64",
            )
            rc = fetch_skia.main(
                ["fetch_skia_for_release.py", "darwin-arm64"]
            )
            self.assertEqual(rc, 1, "sha256 mismatch must fail")


if __name__ == "__main__":
    # Silence the script's progress prints during tests — unittest's own
    # output is what we want to see.
    _real_stdout = sys.stdout
    sys.stdout = io.StringIO()
    try:
        result = unittest.main(verbosity=2, exit=False).result
    finally:
        sys.stdout = _real_stdout
    print(
        f"\nfetch_skia_for_release tests: "
        f"ran {result.testsRun}, "
        f"failures={len(result.failures)}, "
        f"errors={len(result.errors)}"
    )
    sys.exit(0 if result.wasSuccessful() else 1)
