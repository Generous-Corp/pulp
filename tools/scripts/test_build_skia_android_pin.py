"""Regression checks for Android's source-authoritative Skia builder."""

from __future__ import annotations

import json
from pathlib import Path
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
        self.assertIn('cmake --build "$SKIA_SRC/$BUILD_DIR/cmake_dawn" --target webgpu_dawn', script)
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


if __name__ == "__main__":
    unittest.main()
