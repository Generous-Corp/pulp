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
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

SCRIPT = pathlib.Path(__file__).parent / "fetch_skia_for_release.py"

spec = importlib.util.spec_from_file_location("fetch_skia_for_release", SCRIPT)
fetch_skia = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch_skia)


def _make_zip(zip_path: pathlib.Path, members: dict[str, bytes]) -> str:
    """Build a zip with the given member→bytes mapping and return its sha256."""
    members = dict(members)
    # Successful native release fixtures model the complete Skia/Dawn bundle.
    # Individual corruption tests construct incomplete destinations directly.
    for name in tuple(members):
        if name.endswith("/libskia.a"):
            members.setdefault(name.rsplit("/", 1)[0] + "/libdawn_combined.a", b"dawn")
        elif name.endswith("/skia.lib"):
            members.setdefault(name.rsplit("/", 1)[0] + "/dawn_combined.lib", b"dawn")
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_STORED) as zf:
        for name, data in members.items():
            zf.writestr(name, data)
    h = hashlib.sha256()
    with zip_path.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


class _EncodingCheckedStream(io.StringIO):
    def write(self, value: str) -> int:
        value.encode("cp1252")
        return super().write(value)


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

    def test_wasm_matrix_uses_manifest_asset_key(self):
        self.assertEqual(fetch_skia.MATRIX_TO_MANIFEST["wasm"], "wasm-wasm32")

    def test_unknown(self):
        with self.assertRaises(SystemExit):
            fetch_skia.expected_library_path("haiku-ppc")

    def test_default_dest_root_unchanged(self):
        # The release lane calls with no --dest; the path must stay under
        # external/skia-build (backward-compatible default).
        p = fetch_skia.expected_library_path("darwin-arm64")
        self.assertTrue(str(p).startswith("external/skia-build/"))

    def test_custom_dest_root(self):
        # The shared-cache auto-provision passes --dest; the library path must
        # be re-rooted there while keeping the build/<plat>-gpu/... layout.
        p = fetch_skia.expected_library_path(
            "darwin-arm64", "/home/u/.cache/pulp/skia-build"
        )
        self.assertEqual(
            str(p),
            "/home/u/.cache/pulp/skia-build/build/mac-gpu/lib/Release/libskia.a",
        )

    def test_main_rejects_dest_without_value(self):
        self.assertEqual(
            fetch_skia.main(["fetch_skia_for_release.py", "darwin-arm64", "--dest"]),
            2,
        )

    def test_main_rejects_nonpositive_lock_timeout(self):
        self.assertEqual(fetch_skia.main([
            "fetch_skia_for_release.py", "darwin-arm64",
            "--cache-lock-timeout", "0",
        ]), 2)

    def test_main_rejects_nan_and_infinite_lock_timeout(self):
        for value in ("nan", "inf", "-inf"):
            with self.subTest(value=value):
                self.assertEqual(fetch_skia.main([
                    "fetch_skia_for_release.py", "darwin-arm64",
                    "--cache-lock-timeout", value,
                ]), 2)


class CachePublicationLock(unittest.TestCase):
    def test_live_owner_times_out_bounded(self):
        with _in_tempdir() as td:
            dest = td / "cache" / "skia-build"
            with fetch_skia.cache_lock(str(dest), 1):
                with self.assertRaises(TimeoutError):
                    with fetch_skia.cache_lock(str(dest), 0.05):
                        pass

    def test_dead_same_host_owner_is_recovered(self):
        with _in_tempdir() as td:
            dest = td / "cache" / "skia-build"
            lock = dest.parent / ".skia-build.fetch.lock"
            lock.mkdir(parents=True)
            (lock / "owner.json").write_text(json.dumps({
                "pid": 999_999_999,
                "host": fetch_skia.socket.gethostname(),
                "dest": str(dest),
            }))
            with fetch_skia.cache_lock(str(dest), 1):
                self.assertTrue(lock.is_dir())
            self.assertFalse(lock.exists())

    def test_waiter_rechecks_published_stamp_and_skips_download(self):
        with _in_tempdir() as td:
            source = td / "skia.zip"
            sha = _make_zip(
                source, {
                    "build/mac-gpu/lib/Release/libskia.a": b"shared",
                    "build/mac-gpu/lib/Release/libdawn_combined.a": b"dawn",
                }
            )
            _write_manifest(td, f"file://{source.as_posix()}", sha, "mac-arm64")
            argv = ["fetch", "darwin-arm64", "--dest", str(td / "shared"),
                    "--cache-lock-timeout", "1"]
            self.assertEqual(fetch_skia.main(argv), 0)
            source.unlink()
            self.assertEqual(fetch_skia.main(argv), 0)


class ImmutableKeyedCache(unittest.TestCase):
    def _asset(self, root: pathlib.Path, payload: bytes) -> tuple[pathlib.Path, str]:
        source = root / f"skia-{payload.decode()}.zip"
        sha = _make_zip(
            source, {
                "build/mac-gpu/lib/Release/libskia.a": payload,
                "build/mac-gpu/lib/Release/libdawn_combined.a": b"dawn-" + payload,
            }
        )
        _write_manifest(root, f"file://{source.as_posix()}", sha, "mac-arm64")
        return source, sha

    def test_platform_and_asset_sha_select_immutable_generation(self):
        a = fetch_skia.keyed_cache_dest("/cache", "darwin-arm64", "a" * 64)
        b = fetch_skia.keyed_cache_dest("/cache", "darwin-arm64", "b" * 64)
        c = fetch_skia.keyed_cache_dest("/cache", "linux-x64", "a" * 64)
        self.assertNotEqual(a, b)
        self.assertNotEqual(a, c)

    def test_private_stage_is_atomically_published_and_removed(self):
        with _in_tempdir() as td:
            _, sha = self._asset(td, b"v1")
            root = td / "cache"
            rc = fetch_skia.main(["fetch", "darwin-arm64", "--cache-root", str(root),
                                  "--cache-lock-timeout", "1"])
            self.assertEqual(rc, 0)
            dest = fetch_skia.keyed_cache_dest(str(root), "darwin-arm64", sha)
            self.assertTrue(fetch_skia.cache_generation_valid(dest, "darwin-arm64", sha))
            self.assertEqual(list(root.glob(".*.staging-*")), [])

    def test_pin_rotation_retains_old_generation_and_publishes_new(self):
        with _in_tempdir() as td:
            _, sha1 = self._asset(td, b"v1")
            root = td / "cache"
            argv = ["fetch", "darwin-arm64", "--cache-root", str(root),
                    "--cache-lock-timeout", "1"]
            self.assertEqual(fetch_skia.main(argv), 0)
            old = fetch_skia.keyed_cache_dest(str(root), "darwin-arm64", sha1)
            _, sha2 = self._asset(td, b"v2")
            self.assertEqual(fetch_skia.main(argv), 0)
            new = fetch_skia.keyed_cache_dest(str(root), "darwin-arm64", sha2)
            self.assertTrue(old.is_dir())
            self.assertTrue(new.is_dir())
            self.assertEqual(
                (old / "build/mac-gpu/lib/Release/libskia.a").read_bytes(), b"v1"
            )
            self.assertEqual(
                (new / "build/mac-gpu/lib/Release/libskia.a").read_bytes(), b"v2"
            )

    def test_two_cold_publishers_converge_on_one_complete_generation(self):
        with _in_tempdir() as td:
            _, sha = self._asset(td, b"concurrent")
            root = td / "cache"
            argv = [sys.executable, str(SCRIPT), "darwin-arm64", "--cache-root",
                    str(root), "--cache-lock-timeout", "5"]
            first = subprocess.Popen(argv, cwd=td, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True)
            second = subprocess.Popen(argv, cwd=td, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True)
            first_out, first_err = first.communicate(timeout=10)
            second_out, second_err = second.communicate(timeout=10)
            self.assertEqual(first.returncode, 0, first_err + first_out)
            self.assertEqual(second.returncode, 0, second_err + second_out)
            dest = fetch_skia.keyed_cache_dest(str(root), "darwin-arm64", sha)
            self.assertTrue(fetch_skia.cache_generation_valid(dest, "darwin-arm64", sha))
            self.assertEqual(len([p for p in root.iterdir() if not p.name.startswith(".")]), 1)

    def test_invalid_existing_immutable_generation_is_not_mutated(self):
        with _in_tempdir() as td:
            _, sha = self._asset(td, b"valid")
            root = td / "cache"
            dest = fetch_skia.keyed_cache_dest(str(root), "darwin-arm64", sha)
            dest.mkdir(parents=True)
            sentinel = dest / "do-not-mutate"
            sentinel.write_text("retained")
            rc = fetch_skia.main(["fetch", "darwin-arm64", "--cache-root", str(root),
                                  "--cache-lock-timeout", "1"])
            self.assertEqual(rc, 1)
            self.assertEqual(sentinel.read_text(), "retained")

    def test_generation_rejects_missing_dawn_archive(self):
        with _in_tempdir() as td:
            sha = "a" * 64
            dest = fetch_skia.keyed_cache_dest(str(td), "darwin-arm64", sha)
            lib = fetch_skia.expected_library_path("darwin-arm64", str(dest))
            lib.parent.mkdir(parents=True)
            lib.write_bytes(b"skia")
            (dest / ".skia-asset-sha256").write_text(sha)
            self.assertFalse(fetch_skia.cache_generation_valid(dest, "darwin-arm64", sha))

    def test_direct_warm_hit_rejects_missing_dawn_and_repairs(self):
        with _in_tempdir() as td:
            _, sha = self._asset(td, b"repair")
            dest = td / "baked"
            skia = fetch_skia.expected_library_path("darwin-arm64", str(dest))
            skia.parent.mkdir(parents=True)
            skia.write_bytes(b"skia-only")
            (dest / ".skia-asset-sha256").write_text(sha)
            self.assertEqual(
                fetch_skia.main(["fetch", "darwin-arm64", "--dest", str(dest)]), 0
            )
            self.assertTrue(
                fetch_skia.cache_generation_valid(dest, "darwin-arm64", sha)
            )

    def test_archive_path_traversal_is_rejected_before_writing(self):
        with _in_tempdir() as td:
            source = td / "malicious.zip"
            sha = _make_zip(
                source,
                {
                    "build/mac-gpu/lib/Release/libskia.a": b"skia",
                    "../../outside-cache": b"escaped",
                },
            )
            _write_manifest(td, f"file://{source.as_posix()}", sha, "mac-arm64")
            rc = fetch_skia.main(
                ["fetch", "darwin-arm64", "--dest", str(td / "cache")]
            )
            self.assertEqual(rc, 1)
            self.assertFalse((td / "outside-cache").exists())

    def test_validate_only_accepts_complete_and_rejects_partial_destination(self):
        with _in_tempdir() as td:
            _, sha = self._asset(td, b"validated")
            dest = td / "release-bundle"
            self.assertEqual(
                fetch_skia.main(["fetch", "darwin-arm64", "--dest", str(dest)]), 0
            )
            self.assertEqual(
                fetch_skia.main(
                    ["fetch", "darwin-arm64", "--dest", str(dest), "--validate-only"]
                ),
                0,
            )
            dawn = fetch_skia.expected_dawn_library_path("darwin-arm64", str(dest))
            assert dawn is not None
            dawn.unlink()
            self.assertEqual(
                fetch_skia.main(
                    ["fetch", "darwin-arm64", "--dest", str(dest), "--validate-only"]
                ),
                1,
            )

    def test_generation_rejects_lfs_pointer_archive(self):
        with _in_tempdir() as td:
            sha = "b" * 64
            dest = fetch_skia.keyed_cache_dest(str(td), "darwin-arm64", sha)
            skia = fetch_skia.expected_library_path("darwin-arm64", str(dest))
            dawn = fetch_skia.expected_dawn_library_path("darwin-arm64", str(dest))
            skia.parent.mkdir(parents=True)
            skia.write_text("version https://git-lfs.github.com/spec/v1\n")
            assert dawn is not None
            dawn.write_bytes(b"dawn")
            (dest / ".skia-asset-sha256").write_text(sha)
            self.assertFalse(fetch_skia.cache_generation_valid(dest, "darwin-arm64", sha))

    def test_ios_device_arm64_keeps_arch_subdir(self):
        # Device + simulator zips share build/ios-gpu/, so the arch subdir
        # under Release/ must be preserved (not flattened).
        p = fetch_skia.expected_library_path("ios-device-arm64")
        self.assertEqual(
            str(p),
            "external/skia-build/build/ios-gpu/lib/Release/device-arm64/libskia.a",
        )

    def test_ios_simulator_keeps_arch_subdir(self):
        # The fat simulator zip ships both simulator-arm64 and
        # simulator-x86_64; the sanity-check target is the arm64 slice.
        p = fetch_skia.expected_library_path("ios-simulator-arm64-x86_64")
        self.assertEqual(
            str(p),
            "external/skia-build/build/ios-gpu/lib/Release/simulator-arm64/libskia.a",
        )


class IosMatrixRegistration(unittest.TestCase):
    """iOS matrix slices must be wired and arch-preserving."""

    def test_ios_keys_present_in_matrix_map(self):
        self.assertIn("ios-device-arm64", fetch_skia.MATRIX_TO_MANIFEST)
        self.assertIn(
            "ios-simulator-arm64-x86_64", fetch_skia.MATRIX_TO_MANIFEST
        )

    def test_ios_keys_in_preserve_set(self):
        # The flatten step would collide device-arm64 / simulator-arm64
        # / simulator-x86_64 libskia.a copies into one Release/ dir.
        self.assertIn(
            "ios-device-arm64", fetch_skia._IOS_PRESERVE_ARCH_SUBDIR
        )
        self.assertIn(
            "ios-simulator-arm64-x86_64",
            fetch_skia._IOS_PRESERVE_ARCH_SUBDIR,
        )


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

    def test_known_platform_without_asset_reports_matrix_and_manifest_key(self):
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

            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = fetch_skia.main(
                    ["fetch_skia_for_release.py", "windows-arm64"]
                )

        self.assertEqual(rc, 0)
        self.assertIn("matrix=windows-arm64", out.getvalue())
        self.assertIn("manifest key 'win-arm64'", out.getvalue())


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
            self.assertFalse((td / "skia-release-asset.zip").exists())


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

    def test_windows_x64_arch_subdir_with_cp1252_stdout(self):
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

            with contextlib.redirect_stdout(_EncodingCheckedStream()):
                rc = fetch_skia.main(
                    ["fetch_skia_for_release.py", "windows-x64"]
                )

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

    def test_arch_subdir_with_only_duplicate_flat_file_leaves_subdir(self):
        with _in_tempdir() as td:
            zip_path = td / "skia-mac.zip"
            payload = {
                "build/mac-gpu/lib/Release/libskia.a": b"already-flat",
                "build/mac-gpu/lib/Release/arm64/libskia.a": b"duplicate",
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
            self.assertEqual((release_dir / "libskia.a").read_bytes(), b"already-flat")
            self.assertTrue((release_dir / "arm64" / "libskia.a").is_file())

    def test_empty_arch_subdir_is_removed_before_missing_lib_error(self):
        with _in_tempdir() as td:
            zip_path = td / "skia-mac.zip"
            payload = {
                "build/mac-gpu/lib/Release/arm64/": b"",
                "build/mac-gpu/lib/Release/README.txt": b"no libs here",
            }
            sha = _make_zip(zip_path, payload)
            _write_manifest(
                td, f"file://{zip_path.as_posix()}", sha, "mac-arm64"
            )

            rc = fetch_skia.main(
                ["fetch_skia_for_release.py", "darwin-arm64"]
            )

            self.assertEqual(rc, 1)
            release_dir = (
                td / "external/skia-build/build/mac-gpu/lib/Release"
            )
            self.assertFalse((release_dir / "arm64").exists())


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

    def test_no_lib_prints_directory_listing(self):
        with _in_tempdir() as td:
            zip_path = td / "skia-empty.zip"
            payload = {
                "build/mac-gpu/lib/Release/README.txt": b"no libs here",
            }
            sha = _make_zip(zip_path, payload)
            _write_manifest(
                td, f"file://{zip_path.as_posix()}", sha, "mac-arm64"
            )

            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = fetch_skia.main(
                    ["fetch_skia_for_release.py", "darwin-arm64"]
                )

        self.assertEqual(rc, 1)
        self.assertIn(
            "expected materialized Skia/Dawn archives not found", err.getvalue()
        )
        self.assertIn("README.txt", err.getvalue())


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


class PrivateArchiveCleanup(unittest.TestCase):
    def assert_no_private_archive(self, root: pathlib.Path) -> None:
        self.assertEqual(list(root.glob(".skia-release-asset-*.zip")), [])

    def test_network_failure_unlinks_unique_archive(self):
        with _in_tempdir() as td:
            _write_manifest(td, "https://example.invalid/skia.zip", "0" * 64,
                            "mac-arm64")
            with mock.patch.object(fetch_skia.urllib.request, "urlopen",
                                   side_effect=OSError("network dropped")):
                with self.assertRaisesRegex(OSError, "network dropped"):
                    fetch_skia.main(["fetch", "darwin-arm64"])
            self.assert_no_private_archive(td)

    def test_invalid_zip_unlinks_unique_archive(self):
        with _in_tempdir() as td:
            source = td / "not-a-zip.bin"
            source.write_bytes(b"not a zip")
            sha = hashlib.sha256(source.read_bytes()).hexdigest()
            _write_manifest(td, f"file://{source.as_posix()}", sha, "mac-arm64")
            with self.assertRaises(zipfile.BadZipFile):
                fetch_skia.main(["fetch", "darwin-arm64"])
            self.assert_no_private_archive(td)

    def test_extraction_failure_unlinks_unique_archive(self):
        with _in_tempdir() as td:
            source = td / "skia.zip"
            sha = _make_zip(
                source, {"build/mac-gpu/lib/Release/libskia.a": b"skia"}
            )
            _write_manifest(td, f"file://{source.as_posix()}", sha, "mac-arm64")
            original_open = zipfile.ZipFile.open

            def fail_member_open(self, name, *args, **kwargs):
                if not isinstance(name, str):
                    raise OSError("extract failed")
                return original_open(self, name, *args, **kwargs)

            with mock.patch.object(fetch_skia.zipfile.ZipFile, "open",
                                   new=fail_member_open):
                with self.assertRaisesRegex(OSError, "extract failed"):
                    fetch_skia.main(["fetch", "darwin-arm64"])
            self.assert_no_private_archive(td)

    def test_late_validation_failure_unlinks_unique_archive(self):
        with _in_tempdir() as td:
            source = td / "skia.zip"
            sha = _make_zip(
                source, {"build/mac-gpu/lib/Release/README.txt": b"no library"}
            )
            _write_manifest(td, f"file://{source.as_posix()}", sha, "mac-arm64")
            self.assertEqual(fetch_skia.main(["fetch", "darwin-arm64"]), 1)
            self.assert_no_private_archive(td)


class IdempotencyStamp(unittest.TestCase):
    """The `.skia-asset-sha256` stamp prevents stale cached Skia.

    A self-hosted CI runner checks out `clean: false`, so a prior fetch's
    `external/skia-build/` persists. The fetch must be skipped when the
    on-disk Skia matches the pinned asset, but MUST re-run when the
    manifest pin changes — a stale local libskia.a silently shadowing a
    new pin is exactly the non-reproducibility bug the stamp prevents.
    """

    def test_first_run_writes_stamp(self):
        with _in_tempdir() as td:
            zip_path = td / "skia.zip"
            sha = _make_zip(
                zip_path, {"build/mac-gpu/lib/Release/libskia.a": b"skia-v1"}
            )
            _write_manifest(td, f"file://{zip_path.as_posix()}", sha, "mac-arm64")

            rc = fetch_skia.main(["fetch_skia_for_release.py", "darwin-arm64"])

            self.assertEqual(rc, 0)
            stamp = td / "external/skia-build/.skia-asset-sha256"
            self.assertTrue(stamp.is_file(), "fetch must write the stamp")
            self.assertEqual(stamp.read_text(encoding="utf-8").strip(), sha)

    def test_second_run_skips_download_when_stamp_matches(self):
        with _in_tempdir() as td:
            zip_path = td / "skia.zip"
            sha = _make_zip(
                zip_path, {"build/mac-gpu/lib/Release/libskia.a": b"skia-v1"}
            )
            _write_manifest(td, f"file://{zip_path.as_posix()}", sha, "mac-arm64")
            self.assertEqual(
                fetch_skia.main(["fetch_skia_for_release.py", "darwin-arm64"]), 0
            )

            # Delete the asset source — a download attempt would now fail.
            # A correct skip leaves rc == 0.
            zip_path.unlink()
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = fetch_skia.main(
                    ["fetch_skia_for_release.py", "darwin-arm64"]
                )

            self.assertEqual(rc, 0, "matching stamp must skip the download")
            self.assertIn("skipping download", out.getvalue())

    def test_pin_change_forces_refetch(self):
        with _in_tempdir() as td:
            zip_v1 = td / "skia-v1.zip"
            sha_v1 = _make_zip(
                zip_v1, {"build/mac-gpu/lib/Release/libskia.a": b"skia-v1"}
            )
            _write_manifest(
                td, f"file://{zip_v1.as_posix()}", sha_v1, "mac-arm64"
            )
            self.assertEqual(
                fetch_skia.main(["fetch_skia_for_release.py", "darwin-arm64"]), 0
            )
            lib = td / "external/skia-build/build/mac-gpu/lib/Release/libskia.a"
            self.assertEqual(lib.read_bytes(), b"skia-v1")

            # Bump the manifest pin to a different asset.
            zip_v2 = td / "skia-v2.zip"
            sha_v2 = _make_zip(
                zip_v2,
                {"build/mac-gpu/lib/Release/libskia.a": b"skia-v2-different"},
            )
            self.assertNotEqual(sha_v1, sha_v2)
            _write_manifest(
                td, f"file://{zip_v2.as_posix()}", sha_v2, "mac-arm64"
            )

            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = fetch_skia.main(
                    ["fetch_skia_for_release.py", "darwin-arm64"]
                )

            self.assertEqual(rc, 0)
            self.assertEqual(
                lib.read_bytes(),
                b"skia-v2-different",
                "stale Skia must be replaced when the manifest pin changes",
            )
            stamp = td / "external/skia-build/.skia-asset-sha256"
            self.assertEqual(stamp.read_text(encoding="utf-8").strip(), sha_v2)

    def test_missing_lib_with_stamp_refetches(self):
        with _in_tempdir() as td:
            zip_path = td / "skia.zip"
            sha = _make_zip(
                zip_path, {"build/mac-gpu/lib/Release/libskia.a": b"skia-v1"}
            )
            _write_manifest(td, f"file://{zip_path.as_posix()}", sha, "mac-arm64")
            self.assertEqual(
                fetch_skia.main(["fetch_skia_for_release.py", "darwin-arm64"]), 0
            )

            # A wiped/partial workspace: stamp survives, library is gone.
            lib = td / "external/skia-build/build/mac-gpu/lib/Release/libskia.a"
            lib.unlink()

            rc = fetch_skia.main(["fetch_skia_for_release.py", "darwin-arm64"])

            self.assertEqual(rc, 0)
            self.assertTrue(
                lib.is_file(),
                "a missing library must re-fetch even when the stamp exists",
            )

    def test_missing_stamp_uses_matching_version_doc(self):
        with _in_tempdir() as td:
            sha = "a" * 64
            asset_url = (
                "https://github.com/danielraffel/skia-builder/releases/download/"
                "chrome/m149/skia-build-mac-arm64-gpu-release.zip"
            )
            _write_manifest(td, asset_url, sha, "mac-arm64")

            lib = td / fetch_skia.expected_library_path("darwin-arm64")
            lib.parent.mkdir(parents=True)
            lib.write_bytes(b"legacy-cache")
            dawn = td / fetch_skia.expected_dawn_library_path("darwin-arm64")
            dawn.write_bytes(b"legacy-dawn-cache")
            version = td / "external/skia-build/VERSION.md"
            version.write_text(
                "\n".join(
                    [
                        "## Release Asset Digests",
                        "| Asset | SHA-256 |",
                        "|-------|---------|",
                        f"| `skia-build-mac-arm64-gpu-release.zip` | `{sha}` |",
                    ]
                ),
                encoding="utf-8",
            )

            def _boom(*args, **kwargs):
                raise AssertionError("matching VERSION.md must skip download")

            out = io.StringIO()
            with mock.patch.object(
                fetch_skia.urllib.request, "urlopen", side_effect=_boom
            ), contextlib.redirect_stdout(out):
                rc = fetch_skia.main(
                    ["fetch_skia_for_release.py", "darwin-arm64"]
                )

            self.assertEqual(rc, 0)
            stamp = td / "external/skia-build/.skia-asset-sha256"
            self.assertEqual(stamp.read_text(encoding="utf-8").strip(), sha)
            self.assertIn("VERSION.md records", out.getvalue())

    def test_version_doc_digest_mismatch_refetches(self):
        with _in_tempdir() as td:
            zip_path = td / "skia.zip"
            sha = _make_zip(
                zip_path, {"build/mac-gpu/lib/Release/libskia.a": b"fresh"}
            )
            _write_manifest(td, f"file://{zip_path.as_posix()}", sha, "mac-arm64")

            lib = td / fetch_skia.expected_library_path("darwin-arm64")
            lib.parent.mkdir(parents=True)
            lib.write_bytes(b"legacy-cache")
            version = td / "external/skia-build/VERSION.md"
            version.write_text(
                "| `skia.zip` | `" + ("0" * 64) + "` |\n",
                encoding="utf-8",
            )

            rc = fetch_skia.main(["fetch_skia_for_release.py", "darwin-arm64"])

            self.assertEqual(rc, 0)
            self.assertEqual(lib.read_bytes(), b"fresh")
            stamp = td / "external/skia-build/.skia-asset-sha256"
            self.assertEqual(stamp.read_text(encoding="utf-8").strip(), sha)


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
