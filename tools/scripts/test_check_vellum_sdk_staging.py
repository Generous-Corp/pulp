#!/usr/bin/env python3
import re
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent))
import check_vellum_sdk_staging as checker

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = (REPO / ".github/workflows/release-cli.yml").read_text(encoding="utf-8")


class CheckVellumSdkStagingTests(unittest.TestCase):
    def make_prefix(self) -> Path:
        root = Path(self.tmp.name)
        (root / "include/pulp/vellum").mkdir(parents=True)
        (root / "include/pulp/vellum/editor.hpp").write_text("#pragma once\n", encoding="utf-8")
        (root / "lib").mkdir()
        (root / "lib/libpulp-vellum.a").write_bytes(b"archive")
        targets = root / "lib/cmake/Pulp/PulpTargets.cmake"
        targets.parent.mkdir(parents=True)
        targets.write_text("add_library(Pulp::vellum STATIC IMPORTED)\n", encoding="utf-8")
        package = root / "share/pulp/vellum-sdk/0.2.2/sdk/lib/cmake/Vellum/VellumConfig.cmake"
        package.parent.mkdir(parents=True)
        package.write_text("# Vellum package\n", encoding="utf-8")
        return root

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_required_surface_passes(self) -> None:
        self.assertEqual(checker.check(self.make_prefix()), [])

    def test_missing_header_fails(self) -> None:
        prefix = self.make_prefix()
        (prefix / "include/pulp/vellum/editor.hpp").unlink()
        self.assertIn("include/pulp/vellum/editor.hpp is missing", checker.check(prefix))

    def test_empty_header_directory_fails(self) -> None:
        prefix = self.make_prefix()
        for path in (prefix / "include/pulp/vellum").iterdir():
            path.unlink()
        self.assertIn("include/pulp/vellum/editor.hpp is missing", checker.check(prefix))

    def test_missing_library_fails(self) -> None:
        prefix = self.make_prefix()
        (prefix / "lib/libpulp-vellum.a").unlink()
        self.assertIn("lib/libpulp-vellum.a is missing", checker.check(prefix))

    def test_placeholder_library_name_fails(self) -> None:
        prefix = self.make_prefix()
        (prefix / "lib/libpulp-vellum.a").unlink()
        (prefix / "lib/libpulp-vellum.txt").write_text("not a library", encoding="utf-8")
        self.assertIn("lib/libpulp-vellum.a is missing", checker.check(prefix))

    def test_missing_staged_vellum_package_fails(self) -> None:
        prefix = self.make_prefix()
        next((prefix / "share/pulp/vellum-sdk").rglob("VellumConfig.cmake")).unlink()
        self.assertIn(
            "share/pulp/vellum-sdk/<version>/sdk has no VellumConfig.cmake package",
            checker.check(prefix),
        )

    def test_misplaced_vellum_package_fails(self) -> None:
        prefix = self.make_prefix()
        sdk = next((prefix / "share/pulp/vellum-sdk").glob("*/sdk"))
        next(sdk.rglob("VellumConfig.cmake")).unlink()
        misplaced = sdk / "metadata/VellumConfig.cmake"
        misplaced.parent.mkdir(parents=True)
        misplaced.write_text("# not on the CMake search path\n", encoding="utf-8")
        self.assertIn(
            "share/pulp/vellum-sdk/<version>/sdk has no VellumConfig.cmake package",
            checker.check(prefix),
        )

    def test_missing_target_fails(self) -> None:
        prefix = self.make_prefix()
        (prefix / "lib/cmake/Pulp/PulpTargets.cmake").unlink()
        self.assertIn(
            "the installed CMake package has no Pulp::vellum target", checker.check(prefix)
        )

    def test_audio_only_or_commented_target_fails(self) -> None:
        prefix = self.make_prefix()
        targets = prefix / "lib/cmake/Pulp/PulpTargets.cmake"
        for text in (
            "add_library(Pulp::vellum-audio STATIC IMPORTED)\n",
            "# add_library(Pulp::vellum STATIC IMPORTED)\n",
            "# mentions Pulp::vellum only in a comment\n",
        ):
            targets.write_text(text, encoding="utf-8")
            self.assertIn(
                "the installed CMake package has no Pulp::vellum target",
                checker.check(prefix),
                text,
            )


class ReleaseWorkflowWiringTests(unittest.TestCase):
    """Bind the release-cli.yml wiring the checker relies on."""

    def test_empty_bridge_flags_array_is_bash32_safe(self) -> None:
        self.assertIn('${VELLUM_FLAGS[@]+"${VELLUM_FLAGS[@]}"}', WORKFLOW)
        self.assertNotIn('"${VELLUM_FLAGS[@]}"', WORKFLOW.replace(
            '${VELLUM_FLAGS[@]+"${VELLUM_FLAGS[@]}"}', ""))

    def test_digest_is_verified_before_extraction(self) -> None:
        shasum = WORKFLOW.index('shasum -a 256 -c -')
        extract = WORKFLOW.index('tar -xzf "$VELLUM_ARCHIVE"')
        self.assertLess(shasum, extract)
        self.assertLess(WORKFLOW.index("python3 -I tools/deps/pulp_lock.py"), shasum)
        self.assertIn("--proto '=https'", WORKFLOW)
        self.assertIn('rm -rf "$VELLUM_ROOT"', WORKFLOW)

    def test_bridge_flags_and_staged_checks_are_wired(self) -> None:
        for flag in (
            "-DPULP_ENABLE_PULP_VELLUM=ON",
            "-DPULP_ENABLE_PULP_VELLUM_AUDIO=ON",
            '-DCMAKE_PREFIX_PATH=$VELLUM_ROOT/sdk',
        ):
            self.assertIn(flag, WORKFLOW)
        install = WORKFLOW.index("cmake --install build-sdk --prefix sdk-staging")
        bundle = WORKFLOW.index("tools/scripts/bundle_vellum_sdk.py", install)
        check = WORKFLOW.index("tools/scripts/check_vellum_sdk_staging.py sdk-staging", install)
        self.assertLess(install, bundle)
        self.assertLess(bundle, check)

    def test_release_configures_a_consumer_against_only_the_staged_prefix(self) -> None:
        check = WORKFLOW.index("tools/scripts/check_vellum_sdk_staging.py sdk-staging")
        smoke = WORKFLOW.index("vellum-consumer-smoke", check)
        self.assertIn('-DCMAKE_PREFIX_PATH="$PWD/sdk-staging"', WORKFLOW[smoke:smoke + 2500])
        self.assertIn("find_package(Vellum CONFIG REQUIRED PATHS", WORKFLOW[smoke:smoke + 2500])

    def test_matrix_lists_all_bridge_archives_for_darwin_arm64(self) -> None:
        import json

        matrix = json.loads(
            (REPO / "tools/scripts/release_product_matrix.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            sorted(matrix["platform_library_stems"]["darwin-arm64"]),
            ["pulp-vellum", "pulp-vellum-audio", "pulp-vellum-scripted"],
        )

    def test_config_hints_staged_vellum_and_stays_optional(self) -> None:
        text = (REPO / "tools/cmake/PulpConfig.cmake.in").read_text(encoding="utf-8")
        self.assertIn("share/pulp/vellum-sdk/*/sdk", text)
        self.assertRegex(text, r"find_package\(Vellum [^)]*CONFIG QUIET HINTS")
        self.assertNotIn("find_dependency(Vellum", text)


if __name__ == "__main__":
    unittest.main()
