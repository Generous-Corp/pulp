"""Unit tests for the Skia m153 link-probe discovery and fail-closed paths."""

from __future__ import annotations

import tempfile
from pathlib import Path
import subprocess
import sys
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.scripts import verify_skia_m153_capabilities as probe


class SkiaM153CapabilityProbeTests(unittest.TestCase):
    def test_universal_source_binding_rejects_dirty_checkout(self) -> None:
        dirty = subprocess.CompletedProcess(
            ["git", "status"], 0, " M tools/scripts/verify_skia_m153_capabilities.py\n", ""
        )
        with mock.patch.object(probe.subprocess, "run", return_value=dirty):
            with self.assertRaisesRegex(RuntimeError, "clean exact source checkout"):
                probe._source_sha()

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
            self.assertEqual(
                probe.skia_fetch.expected_library_path("darwin-arm64", str(root)),
                library,
            )

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
        preflight = script.index('resolved_skia_commit="$(git ls-remote')
        build_boundary = script.index('python3 build-skia.py')
        verification = script.index('actual_skia_commit="$(git -C')
        copy_boundary = script.index('echo "Copying build output to $SKIA_BUILD_OUTPUT..."')
        self.assertLess(preflight, build_boundary)
        self.assertLess(verification, copy_boundary)
        self.assertIn('resolved_skia_commit" != "$SKIA_EXPECTED_COMMIT', script)
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

    def test_universal_rejects_intel_host_without_arm64_runtime_proof(self) -> None:
        with mock.patch.object(probe.sys, "platform", "darwin"), \
             mock.patch.object(probe.host_platform, "machine", return_value="x86_64"):
            with self.assertRaisesRegex(RuntimeError, "requires a darwin-arm64 host"):
                probe._require_native_host("darwin-universal")

    def test_universal_result_binds_both_arches_to_one_generation(self) -> None:
        dependency = {
            "version": "chrome/m153",
            "determinism": {
                "release_assets": {"mac-universal": {"sha256": "a" * 64}}
            },
        }
        calls: list[list[str]] = []

        def run(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
            calls.append(command)
            return subprocess.CompletedProcess(command, 0, "", "")

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            header = root / "build/include/include/utils/SkLogHandler.h"
            library = root / "build/mac-gpu/lib/Release/libskia.a"
            receipt = root / probe.skia_fetch.GENERATION_RECEIPT
            header.parent.mkdir(parents=True)
            library.parent.mkdir(parents=True)
            header.write_text("// header\n", encoding="utf-8")
            library.write_bytes(b"fat archive")
            receipt.write_text('{"platform":"darwin-universal"}\n', encoding="utf-8")

            with mock.patch.object(probe, "_manifest_skia", return_value=dependency), \
                 mock.patch.object(probe, "_require_native_host"), \
                 mock.patch.object(probe, "_compiler", return_value="clang++"), \
                 mock.patch.object(probe, "_source_sha", return_value="b" * 40), \
                 mock.patch.object(
                     probe.skia_fetch, "cache_generation_valid", return_value=True
                 ), mock.patch.object(probe.subprocess, "run", side_effect=run):
                result = probe.verify(root, "darwin-universal")

        self.assertEqual(result["source_sha"], "b" * 40)
        self.assertEqual(result["asset_sha256"], "a" * 64)
        self.assertEqual(result["platform"], "darwin-universal")
        self.assertEqual(
            [item["architecture"] for item in result["probes"]],
            ["arm64", "x86_64"],
        )
        compile_calls = [call for call in calls if "-o" in call]
        self.assertEqual(len(compile_calls), 2)
        self.assertIn("arm64", compile_calls[0])
        self.assertIn("x86_64", compile_calls[1])
        self.assertTrue(any(call[:2] == ["/usr/bin/arch", "-x86_64"] for call in calls))

    def test_universal_rejects_failed_rosetta_execution(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            include = root / "include"
            library = root / "libskia.a"
            include.mkdir()
            library.write_bytes(b"fat archive")

            def run(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
                failed = command[:2] == ["/usr/bin/arch", "-x86_64"]
                return subprocess.CompletedProcess(command, 1 if failed else 0, "", "")

            with mock.patch.object(probe.subprocess, "run", side_effect=run):
                with self.assertRaisesRegex(RuntimeError, "x86_64 capability probe exited 1"):
                    probe._compile_and_run(
                        include, library, "clang++", root, "x86_64"
                    )

    def test_universal_rejects_generation_change_after_both_probes(self) -> None:
        dependency = {
            "version": "chrome/m153",
            "determinism": {
                "release_assets": {"mac-universal": {"sha256": "a" * 64}}
            },
        }
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            header = root / "build/include/include/utils/SkLogHandler.h"
            library = root / "build/mac-gpu/lib/Release/libskia.a"
            receipt = root / probe.skia_fetch.GENERATION_RECEIPT
            header.parent.mkdir(parents=True)
            library.parent.mkdir(parents=True)
            header.write_text("// header\n", encoding="utf-8")
            library.write_bytes(b"fat archive")
            receipt.write_text('{"platform":"darwin-universal"}\n', encoding="utf-8")
            passed = {
                "architecture": "arm64",
                "compile": "pass",
                "link": "pass",
                "run": "pass",
                "run_mode": "native",
            }
            with mock.patch.object(probe, "_manifest_skia", return_value=dependency), \
                 mock.patch.object(probe, "_require_native_host"), \
                 mock.patch.object(probe, "_compiler", return_value="clang++"), \
                 mock.patch.object(probe, "_source_sha", return_value="b" * 40), \
                 mock.patch.object(probe, "_compile_and_run", return_value=passed), \
                 mock.patch.object(
                     probe.skia_fetch,
                     "cache_generation_valid",
                     side_effect=[True, False],
                 ):
                with self.assertRaisesRegex(RuntimeError, "generation changed"):
                    probe.verify(root, "darwin-universal")


if __name__ == "__main__":
    unittest.main()
