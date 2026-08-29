"""Unit tests for the Skia m153 link-probe discovery and fail-closed paths."""

from __future__ import annotations

import tempfile
from pathlib import Path
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.scripts import verify_skia_m153_capabilities as probe


class SkiaM153CapabilityProbeTests(unittest.TestCase):
    def test_discovers_published_archive_layout(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            header = root / "build/include/include/utils/SkLogHandler.h"
            library = root / "build/mac-gpu/lib/Release/libskia.a"
            header.parent.mkdir(parents=True)
            library.parent.mkdir(parents=True)
            header.write_text("// header\n", encoding="utf-8")
            library.write_bytes(b"archive")
            self.assertEqual(probe._find_include_root(root), root / "build/include")
            self.assertEqual(probe._find_skia_library(root), library)

    def test_missing_header_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(RuntimeError, "SkLogHandler.h not found"):
                probe._find_include_root(Path(temp))

    def test_wrong_manifest_release_is_rejected_before_compile(self) -> None:
        with mock.patch.object(
            probe, "_manifest_skia", return_value={"version": "chrome/m152"}
        ), mock.patch.object(probe, "_require_native_host"):
            with self.assertRaisesRegex(RuntimeError, r"requires chrome/m153\+"):
                probe.verify(Path("/does/not/matter"), "darwin-arm64")

    def test_later_milestone_reaches_provider_validation(self) -> None:
        dependency = {
            "version": "chrome/m154",
            "determinism": {
                "release_assets": {"mac-arm64": {"sha256": "b" * 64}}
            },
        }
        with mock.patch.object(probe, "_manifest_skia", return_value=dependency), \
             mock.patch.object(probe, "_require_native_host"), \
             mock.patch.object(
                 probe.skia_fetch, "cache_generation_valid", return_value=False
             ):
            with self.assertRaisesRegex(RuntimeError, "verified darwin-arm64"):
                probe.verify(Path("/does/not/matter"), "darwin-arm64")

    def test_source_fallback_fails_before_copy_on_commit_drift(self) -> None:
        script = (probe.REPO_ROOT / "tools/build-skia.sh").read_text(encoding="utf-8")
        self.assertIn('SKIA_EXPECTED_COMMIT="${SKIA_EXPECTED_COMMIT:-$(python3', script)
        verification = script.index('actual_skia_commit="$(git -C')
        copy_boundary = script.index('echo "Copying build output to $SKIA_BUILD_OUTPUT..."')
        self.assertLess(verification, copy_boundary)
        self.assertIn('actual_skia_commit" != "$SKIA_EXPECTED_COMMIT', script)

    def test_unverified_generation_is_rejected_before_compile(self) -> None:
        dependency = {
            "version": "chrome/m153",
            "determinism": {
                "release_assets": {"mac-arm64": {"sha256": "a" * 64}}
            },
        }
        with mock.patch.object(probe, "_manifest_skia", return_value=dependency), \
             mock.patch.object(probe, "_require_native_host"), \
             mock.patch.object(
                 probe.skia_fetch, "cache_generation_valid", return_value=False
             ):
            with self.assertRaisesRegex(RuntimeError, "verified darwin-arm64"):
                probe.verify(Path("/does/not/matter"), "darwin-arm64")

    def test_cross_host_and_non_native_platforms_fail_closed(self) -> None:
        with mock.patch.object(probe.sys, "platform", "darwin"), \
             mock.patch.object(probe.host_platform, "machine", return_value="arm64"):
            probe._require_native_host("darwin-arm64")
            probe._require_native_host("darwin-universal")
            with self.assertRaisesRegex(RuntimeError, "cannot compile and execute"):
                probe._require_native_host("darwin-x64")
            with self.assertRaisesRegex(RuntimeError, "cannot compile and execute"):
                probe._require_native_host("wasm")


if __name__ == "__main__":
    unittest.main()
