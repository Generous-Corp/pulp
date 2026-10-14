"""Regression checks for Android's source-authoritative Skia builder."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class AndroidSkiaBuilderPinTests(unittest.TestCase):
    def test_builder_defaults_to_manifest_provider_pair(self) -> None:
        script = (ROOT / "tools/build-skia-android.sh").read_text(encoding="utf-8")
        manifest = json.loads((ROOT / "tools/deps/manifest.json").read_text(encoding="utf-8"))
        skia = next(item for item in manifest["dependencies"] if item["name"] == "Skia")
        determinism = skia["determinism"]

        self.assertIn('SKIA_BRANCH="${SKIA_BRANCH:-$(read_manifest_pin skia_branch)}"', script)
        self.assertIn(
            'SKIA_EXPECTED_COMMIT="${SKIA_EXPECTED_COMMIT:-$(read_manifest_pin skia_commit)}"',
            script,
        )
        self.assertIn(
            'DAWN_EXPECTED_COMMIT="${DAWN_EXPECTED_COMMIT:-$(read_manifest_pin built_dawn)}"',
            script,
        )
        self.assertEqual(determinism["skia_branch"], "chrome/m153")
        self.assertEqual(len(determinism["skia_commit"]), 40)
        self.assertEqual(len(determinism["built_dawn"]), 40)

    def test_source_and_dependency_checks_precede_android_build(self) -> None:
        script = (ROOT / "tools/build-skia-android.sh").read_text(encoding="utf-8")
        source_check = script.index('ACTUAL_SKIA_COMMIT="$(git -C')
        dawn_check = script.index('ACTUAL_DAWN_COMMIT="$(git -C')
        build_loop = script.index('for ABI in')
        self.assertLess(source_check, dawn_check)
        self.assertLess(dawn_check, build_loop)
        self.assertIn("for mod in skparagraph skshaper skunicode svg skottie sksg", script)
        self.assertIn('cmp -s "$SKUNICODE_HEADER"', script)
        self.assertIn("-DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC", script)
        self.assertIn("cmake --build", script)
        self.assertIn("--target webgpu_dawn", script)
        self.assertIn('cmake_dawn/src/dawn/libdawn_proc.a', script)
        self.assertIn('--localize-symbol="$symbol"', script)
        self.assertIn('grep -q \'src/core/SkUTF.h\'', script)
        self.assertIn('ACTUAL_DAWN_COMMIT" != "$DAWN_EXPECTED_COMMIT"', script)

    def test_android_findskia_drops_unusable_chromium_allocator_archives(self) -> None:
        findskia = (ROOT / "tools/cmake/FindSkia.cmake").read_text(encoding="utf-8")
        self.assertIn('_lib_name STREQUAL "libraw_ptr.a"', findskia)
        self.assertIn('NOT _lib_name STREQUAL "libdawn_proc_compat.a"', findskia)
        self.assertIn('_lib_name MATCHES "^libdawn_proc.*\\\\.a$"', findskia)
        self.assertIn("CMAKE_SYSTEM_NAME STREQUAL \"Linux\" OR ANDROID", findskia)
        self.assertIn("defined(__ANDROID__)", (ROOT / "core/canvas/src/skia_chromium_raw_ptr_compat.cpp").read_text(encoding="utf-8"))
        view_cmake = (ROOT / "core/view/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("elseif(ANDROID)", view_cmake)
        self.assertIn(
            "target_sources(pulp-view-core PRIVATE src/screenshot_skia.cpp)",
            view_cmake,
        )
        self.assertIn("OR (ANDROID AND PULP_HAS_SKIA)", view_cmake)

    def test_android_build_uses_one_governed_lease_per_abi(self) -> None:
        script = (ROOT / "tools/build-skia-android.sh").read_text(encoding="utf-8")
        self.assertIn('PULP_BUILD_CLASS=interactive PULP_ANDROID_SKIA_SRC=', script)
        self.assertEqual(script.count('bash -c "$GOVERNED_ANDROID_BODY"'), 2)
        self.assertIn('ninja -j "$CMAKE_BUILD_PARALLEL_LEVEL"', script)
        self.assertIn('cmake --build "$PULP_ANDROID_SKIA_SRC/$PULP_ANDROID_BUILD_DIR/cmake_dawn"', script)
        self.assertIn('android-build-receipt abi=${DRY_ABI} lease=one-per-abi', script)
        self.assertIn('"$GOVERNED_BUILD" --dry-run bash -c', script)
        # The shared literal body contains both commands; the live path invokes
        # it once per ABI rather than reacquiring a second lease for Dawn.

    def test_dry_run_emits_receipts_without_android_toolchain(self) -> None:
        env = os.environ.copy()
        env.update({"PULP_SKIA_ANDROID_DRY_RUN": "1", "PULP_TARTCI_LEASES": "0"})
        result = subprocess.run(
            ["bash", str(ROOT / "tools/build-skia-android.sh"), "all"],
            cwd=ROOT,
            env=env,
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("android-build-receipt abi=arm64 lease=one-per-abi", result.stdout)
        self.assertIn("android-build-receipt abi=x86_64 lease=one-per-abi", result.stdout)
        self.assertEqual(result.stdout.count("governed-build dry-run receipt:"), 2)
        self.assertIn("ninja -j", result.stdout)
        self.assertIn("cmake --build", result.stdout)

    def test_governed_path_arguments_survive_apostrophe(self) -> None:
        governor = ROOT / "tools/ci/governed-build.sh"
        with tempfile.TemporaryDirectory(prefix="android-skia-") as temporary:
            source = Path(temporary) / "skia'source"
            source.mkdir()
            env = os.environ.copy()
            env.update({"PULP_TARTCI_LEASES": "0", "PULP_ANDROID_SKIA_SRC": str(source)})
            result = subprocess.run(
                [
                    "bash",
                    str(governor),
                    "bash",
                    "-c",
                    'set -e; cd "$PULP_ANDROID_SKIA_SRC"; test "$(pwd)" = "$PULP_ANDROID_SKIA_SRC"',
                ],
                cwd=ROOT,
                env=env,
                text=True,
                encoding="utf-8",
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
