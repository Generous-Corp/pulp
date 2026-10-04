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
        self.assertIn('find "$SKIA_SRC/$BUILD_DIR/cmake_dawn" -name "*.a"', script)
        self.assertIn('grep -q \'src/core/SkUTF.h\'', script)
        self.assertIn('ACTUAL_DAWN_COMMIT" != "$DAWN_EXPECTED_COMMIT"', script)


if __name__ == "__main__":
    unittest.main()
