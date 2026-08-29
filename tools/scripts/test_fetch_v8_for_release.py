#!/usr/bin/env python3
"""
Unit tests for fetch_v8_for_release.py.

Covers the per-platform unpack layout, sha256 verification, the
idempotency stamp (skip when the pin matches, re-fetch when it changes),
and the deliberately runtime-disabled iOS-simulator framework (library: false).

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
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

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


def _matched_pair_fixture() -> tuple[dict, dict]:
    pair = {
        "pair_kind": "chromium-milestone", "milestone": 153,
        "built_revision": "a" * 40, "v8": "a" * 40,
        "chromium_revision": "b" * 40, "chromium_deps_blob": "c" * 40,
        "chromium_branch": "8010", "skia": "d" * 40,
        "built_skia": "e" * 40, "skia_matches_chromium": False,
        "built_dawn": "f" * 40, "dawn": "1" * 40,
        "dawn_matches_chromium": False,
        "validated_skia_release": "chrome/m153",
    }
    pair_sha = hashlib.sha256(
        json.dumps(pair, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    determinism = {
        "pair_kind": pair["pair_kind"], "milestone": pair["milestone"],
        "built_revision": pair["built_revision"],
        "chromium_revision": pair["chromium_revision"],
        "chromium_deps_blob": pair["chromium_deps_blob"],
        "chromium_branch": pair["chromium_branch"],
        "chromium_skia": pair["skia"], "paired_skia": pair["built_skia"],
        "skia_matches_chromium": pair["skia_matches_chromium"],
        "paired_dawn": pair["built_dawn"], "chromium_dawn": pair["dawn"],
        "dawn_matches_chromium": pair["dawn_matches_chromium"],
        "skia_release_tag": pair["validated_skia_release"],
        "pair_manifest_sha256": pair_sha,
    }
    manifest = {"dependencies": [
        {"name": "Skia", "version": "chrome/m153", "determinism": {
            "skia_commit": pair["built_skia"], "built_dawn": pair["built_dawn"]}},
        {"name": "V8", "version": "v8-m153-15.3.76.5-aaaaaaaaaaaa",
         "determinism": determinism},
    ]}
    return manifest, pair


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

    def test_ios_runtime_is_disabled(self):
        self.assertIsNone(fetch_v8.expected_library_path("ios-simulator-arm64"))

    def test_ios_framework_header_is_required(self):
        self.assertEqual(
            str(fetch_v8.expected_header_path("ios-simulator-arm64")),
            "external/v8-build/ios-simulator-arm64/V8.framework/Headers/v8.h",
        )

    def test_matched_windows_requires_runtime_import_and_libcxx(self):
        root = pathlib.Path("sealed")
        required = {
            str(path) for path in fetch_v8.required_materialized_paths(
                root, "win-x64", matched_milestone=True)
        }
        self.assertEqual(required, {
            "sealed/include/v8.h", "sealed/lib/v8.dll",
            "sealed/lib/v8.dll.lib", "sealed/lib/libc++.lib",
        })


class EmbeddedPairManifest(unittest.TestCase):
    def _archive(self, root: pathlib.Path, embedded: dict | None) -> zipfile.ZipFile:
        path = root / "pair.zip"
        with zipfile.ZipFile(path, "w") as zf:
            if embedded is not None:
                zf.writestr("manifest.json", json.dumps(embedded))
        return zipfile.ZipFile(path)

    def test_exact_pair_and_active_provider_pass(self):
        manifest, pair = _matched_pair_fixture()
        with tempfile.TemporaryDirectory() as td, self._archive(
                pathlib.Path(td), {"v8_version": "15.3.76.5", "platform": "mac",
                                   "arch": "arm64", "lib": "lib/libv8.dylib",
                                   "shared": True, "sealed": True, "i18n": True,
                                   "pair": pair}) as zf:
            fetch_v8.validate_embedded_manifest(
                zf, manifest, manifest["dependencies"][1], "mac-arm64")

    def test_missing_embedded_manifest_fails_closed(self):
        manifest, _ = _matched_pair_fixture()
        with tempfile.TemporaryDirectory() as td, self._archive(
                pathlib.Path(td), None) as zf:
            with self.assertRaisesRegex(ValueError, "no embedded manifest"):
                fetch_v8.validate_embedded_manifest(
                    zf, manifest, manifest["dependencies"][1], "mac-arm64")

    def test_pair_or_active_provider_drift_fails_closed(self):
        manifest, pair = _matched_pair_fixture()
        pair["built_skia"] = "9" * 40
        with tempfile.TemporaryDirectory() as td, self._archive(
                pathlib.Path(td), {"v8_version": "15.3.76.5", "platform": "mac",
                                   "arch": "arm64", "lib": "lib/libv8.dylib",
                                   "shared": True, "sealed": True, "i18n": True,
                                   "pair": pair}) as zf:
            with self.assertRaisesRegex(ValueError, "provenance mismatch"):
                fetch_v8.validate_embedded_manifest(
                    zf, manifest, manifest["dependencies"][1], "mac-arm64")

    def test_top_level_artifact_identity_drift_fails_closed(self):
        manifest, pair = _matched_pair_fixture()
        embedded = {"v8_version": "15.3.76.5", "platform": "mac",
                    "arch": "x86_64", "lib": "lib/libv8.dylib",
                    "shared": True, "sealed": True, "i18n": True, "pair": pair}
        with tempfile.TemporaryDirectory() as td, self._archive(
                pathlib.Path(td), embedded) as zf:
            with self.assertRaisesRegex(ValueError, "artifact arch"):
                fetch_v8.validate_embedded_manifest(
                    zf, manifest, manifest["dependencies"][1], "mac-arm64")

    def test_full_fetch_closes_rejected_archive_before_cleanup(self):
        with _in_tempdir() as td:
            manifest, pair = _matched_pair_fixture()
            embedded = {
                "v8_version": "15.3.76.5", "platform": "mac",
                "arch": "x86_64",  # Deliberately wrong for mac-arm64.
                "lib": "lib/libv8.dylib", "shared": True, "sealed": True,
                "i18n": True, "pair": pair,
            }
            zip_path = td / "v8-bad-pair.zip"
            sha = _make_zip(zip_path, {
                "include/v8.h": b"h", "lib/libv8.dylib": b"v8",
                "manifest.json": json.dumps(embedded).encode("utf-8"),
            })
            asset = {"url": f"file://{zip_path.as_posix()}", "sha256": sha}
            manifest["dependencies"][1]["determinism"]["release_assets"] = {
                "mac-arm64": asset}
            (td / "tools/deps").mkdir(parents=True)
            (td / "tools/deps/manifest.json").write_text(
                json.dumps(manifest), encoding="utf-8")
            err = io.StringIO()
            with contextlib.redirect_stderr(err), mock.patch.object(
                    fetch_v8, "verify_release_metadata",
                    return_value="2" * 64):
                rc = fetch_v8.main(
                    ["fetch_v8_for_release.py", "darwin-arm64"])
            self.assertEqual(rc, 1)
            self.assertIn("artifact arch", err.getvalue())
            self.assertFalse(
                (td / "v8-release-asset-mac-arm64.zip").exists(),
                "rejected archive must be closed before Windows-compatible cleanup")

    def test_builder_release_tag_must_resolve_to_pinned_source(self):
        with tempfile.TemporaryDirectory() as td:
            ref_path = pathlib.Path(td) / "ref.json"
            ref_path.write_text(json.dumps({
                "object": {"type": "commit", "sha": "a" * 40}}),
                encoding="utf-8")
            fetch_v8.verify_builder_tag_ref({
                "v8_builder_tag_ref_url": ref_path.as_uri(),
                "v8_builder_ref": "a" * 40,
            })
            with self.assertRaisesRegex(ValueError, "release tag mismatch"):
                fetch_v8.verify_builder_tag_ref({
                    "v8_builder_tag_ref_url": ref_path.as_uri(),
                    "v8_builder_ref": "b" * 40,
                })


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

    def test_windows_full_fetch_is_cp1252_safe(self):
        with _in_tempdir() as td:
            zip_path = td / "v8-win.zip"
            sha = _make_zip(zip_path, {
                "include/v8.h": b"// v8 header", "lib/v8.dll": b"fake-dll",
                "lib/v8.dll.lib": b"fake-implib"})
            _write_manifest(td, f"file://{zip_path.as_posix()}", sha, "win-x64")
            output = io.TextIOWrapper(io.BytesIO(), encoding="cp1252", errors="strict")
            with contextlib.redirect_stdout(output):
                self.assertEqual(
                    fetch_v8.main(["fetch_v8_for_release.py", "windows-x64"]), 0)
            output.flush()


class IosRuntimeDisabled(unittest.TestCase):
    def test_ios_simulator_no_library_succeeds(self):
        with _in_tempdir() as td:
            zip_path = td / "v8-ios.zip"
            sha = _make_zip(zip_path, {
                "V8.framework/Headers/v8.h": b"// v8 header",
                "V8.framework/V8": b"unused-jitless-framework",
                "manifest.json": b"{}",
            })
            _write_manifest(
                td, f"file://{zip_path.as_posix()}", sha,
                "ios-simulator-arm64", library=False,
            )
            rc = fetch_v8.main(["fetch_v8_for_release.py", "ios-simulator-arm64"])
            self.assertEqual(rc, 0, "runtime-disabled iOS provider must validate")
            self.assertTrue((td / "external/v8-build/ios-simulator-arm64/V8.framework/Headers/v8.h").is_file())
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

    def test_mutated_materialized_bytes_force_refetch(self):
        with _in_tempdir() as td:
            zip_path = td / "v8.zip"
            sha = _make_zip(zip_path, {
                "include/v8.h": b"h", "lib/libv8.dylib": b"authentic"})
            _write_manifest(td, f"file://{zip_path.as_posix()}", sha, "mac-arm64")
            self.assertEqual(
                fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"]), 0)
            lib = td / "external/v8-build/mac-arm64/lib/libv8.dylib"
            lib.write_bytes(b"corrupt")
            self.assertEqual(
                fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"]), 0)
            self.assertEqual(lib.read_bytes(), b"authentic")


class BakedV8FastPath(unittest.TestCase):
    """PULP_USE_BAKED_V8 + V8_DIR short-circuit (golden-VM fast path).

    Skips the download only when the baked stamp matches the current pin AND
    the library is present; falls through to a normal fetch otherwise. None of
    these branches were covered before."""

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
            (baked / "mac-arm64" / "include").mkdir(parents=True)
            (baked / "mac-arm64" / "lib" / "libv8.dylib").write_bytes(b"baked")
            (baked / "mac-arm64" / "include" / "v8.h").write_bytes(b"header")
            fetch_v8.write_generation_receipt(
                baked / "mac-arm64", "mac-arm64", sha, None)
            (baked / "mac-arm64" / ".v8-asset-sha256").write_text(sha + "\n")
            out = io.StringIO()
            with self._env(PULP_USE_BAKED_V8="1", V8_DIR=str(baked)), \
                    contextlib.redirect_stdout(out):
                rc = fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"])
            self.assertEqual(rc, 0, "baked match must succeed without fetching")
            self.assertIn("using baked V8", out.getvalue())
            # No download/unpack happened into the checkout.
            self.assertFalse((td / "external/v8-build/mac-arm64/lib/libv8.dylib").exists())

    def test_baked_missing_header_falls_through_to_fetch(self):
        with _in_tempdir() as td:
            zip_path = td / "v8.zip"
            sha = _make_zip(zip_path, {
                "include/v8.h": b"real-header", "lib/libv8.dylib": b"real"})
            _write_manifest(td, f"file://{zip_path.as_posix()}", sha, "mac-arm64")
            baked = td / "baked"
            (baked / "mac-arm64" / "lib").mkdir(parents=True)
            (baked / "mac-arm64" / "lib" / "libv8.dylib").write_bytes(b"baked")
            (baked / "mac-arm64" / ".v8-asset-sha256").write_text(sha + "\n")
            with self._env(PULP_USE_BAKED_V8="1", V8_DIR=str(baked)):
                self.assertEqual(
                    fetch_v8.main(["fetch_v8_for_release.py", "darwin-arm64"]), 0)
            self.assertEqual(
                (td / "external/v8-build/mac-arm64/include/v8.h").read_bytes(),
                b"real-header")

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


class FindV8ReceiptSelection(unittest.TestCase):
    def _provider(self, root: pathlib.Path, asset_sha: str, metadata_sha: str) -> None:
        (root / "include").mkdir(parents=True)
        (root / "lib").mkdir(parents=True)
        (root / "include/v8.h").write_bytes(b"header")
        (root / "lib/libv8.dylib").write_bytes(b"provider")
        (root / "manifest.json").write_text("{}", encoding="utf-8")
        fetch_v8.write_generation_receipt(
            root, "mac-arm64", asset_sha, metadata_sha)
        (root / ".v8-asset-sha256").write_text(asset_sha + "\n", encoding="utf-8")
        (root / ".v8-release-metadata-sha256").write_text(
            metadata_sha + "\n", encoding="utf-8")

    def _configure(self, root: pathlib.Path, baked: pathlib.Path,
                   asset_sha: str, metadata_sha: str) -> pathlib.Path:
        pulp_root = root / "pulp-root"
        manifest = {"dependencies": [{"name": "V8", "determinism": {
            "pair_kind": "chromium-milestone",
            "release_metadata_sha256": metadata_sha,
            "release_assets": {"mac-arm64": {"sha256": asset_sha}},
        }}]}
        (pulp_root / "tools/deps").mkdir(parents=True)
        (pulp_root / "tools/deps/manifest.json").write_text(
            json.dumps(manifest), encoding="utf-8")
        module = SCRIPT.parent.parent / "cmake" / "FindV8.cmake"
        consumer = root / "consumer"
        consumer.mkdir()
        (consumer / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(find_v8_receipt NONE)\n"
            f"set(PULP_ROOT_DIR \"{pulp_root.as_posix()}\")\n"
            "set(APPLE TRUE)\nset(IOS FALSE)\nset(ANDROID FALSE)\n"
            "set(CMAKE_SYSTEM_PROCESSOR arm64)\n"
            "set(CMAKE_OSX_ARCHITECTURES arm64)\n"
            f"include(\"{module.as_posix()}\")\n"
            "if(NOT PULP_V8_FOUND)\nmessage(FATAL_ERROR \"no verified V8\")\nendif()\n"
            "file(WRITE \"${CMAKE_BINARY_DIR}/selected.txt\" \"${V8_RUNTIME_LIBRARY}\")\n",
            encoding="utf-8")
        build = root / "build"
        env = dict(os.environ)
        env["V8_DIR"] = str(baked)
        completed = subprocess.run(
            ["cmake", "-S", str(consumer), "-B", str(build)], env=env,
            text=True, capture_output=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        return pathlib.Path((build / "selected.txt").read_text(encoding="utf-8"))

    def test_stale_baked_provider_cannot_outrank_verified_checkout(self):
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            asset_sha, metadata_sha = "a" * 64, "b" * 64
            checkout = root / "pulp-root/external/v8-build/mac-arm64"
            baked = root / "baked"
            self._provider(checkout, asset_sha, metadata_sha)
            self._provider(baked / "mac-arm64", asset_sha, metadata_sha)
            (baked / "mac-arm64/lib/libv8.dylib").write_bytes(b"mutated")
            selected = self._configure(root, baked, asset_sha, metadata_sha)
            self.assertEqual(selected, checkout / "lib/libv8.dylib")


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
