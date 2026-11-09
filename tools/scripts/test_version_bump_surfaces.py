#!/usr/bin/env python3
"""Fixture tests for the version_bump Surface cluster.

Config loading + version-file readers/writers. Mirrors
`version_bump_surfaces.py`.

Runs standalone (`python3 tools/scripts/test_version_bump_surfaces.py`)
or as part of the aggregate suite via `test_gates.py`.
"""

from __future__ import annotations

import json
import re
import subprocess
import unittest
from pathlib import Path
from unittest import mock

from gate_test_support import GateFixtureTestCase, VBC, _run


class VersionBumpSurfacesTests(GateFixtureTestCase):
    """version_bump_check Surface-cluster fixtures."""

    def test_version_file_helpers_cover_supported_kinds(self) -> None:
        """Direct coverage for the version-file readers/writers that
        integration fixtures only hit through the CMake + JSON paths."""
        vbc = self._import_gate_module("version_bump_check")

        cases = [
            (
                "package.json",
                '{"name": "demo", "version": "1.2.3"}\n',
                vbc.VersionFile("package.json", "json_field", "version"),
                "1.2.3",
                "2.0.0",
                '"version": "2.0.0"',
            ),
            (
                "pyproject.toml",
                '[project]\nname = "demo"\nversion = "0.4.0"\n',
                vbc.VersionFile("pyproject.toml", "pyproject_version"),
                "0.4.0",
                "0.5.0",
                'version = "0.5.0"',
            ),
            (
                "pkg/__init__.py",
                '__version__ = "3.2.1"\n',
                vbc.VersionFile("pkg/__init__.py", "python_dunder_version"),
                "3.2.1",
                "3.2.2",
                '__version__ = "3.2.2"',
            ),
            (
                "VERSION.txt",
                "tool_version = 7.8.9\n",
                vbc.VersionFile("VERSION.txt", "regex",
                                pattern=r"tool_version\s*=\s*(\d+\.\d+\.\d+)"),
                "7.8.9",
                "8.0.0",
                "tool_version = 8.0.0",
            ),
        ]

        for rel, text, vf, old, new, expected in cases:
            self.f.write(rel, text)
            self.assertEqual(vbc.read_version(self.tmp, vf), old)
            self.assertTrue(vbc.write_version(self.tmp, vf, new))
            written = (self.tmp / rel).read_text()
            self.assertIn(expected, written)

        self.f.write("bad.json", "{not json}\n")
        self.assertIsNone(
            vbc.read_version(self.tmp, vbc.VersionFile("bad.json", "json_field", "version"))
        )
        self.assertFalse(
            vbc.write_version(
                self.tmp,
                vbc.VersionFile("missing.toml", "pyproject_version"),
                "1.0.0",
            )
        )

    def test_json_path_read_write_and_walk_guards(self) -> None:
        vbc = self._import_gate_module("version_bump_check")

        self.f.write(
            "manifest.json",
            json.dumps({
                "plugins": [
                    {"name": "alpha", "version": "1.0.0"},
                    {"name": "beta", "version": "2.0.0"},
                ],
                "metadata": {"tool": {"version": "3.0.0"}},
            }, indent=2) + "\n",
        )

        plugin_vf = vbc.VersionFile("manifest.json", "json_path", "plugins.1.version")
        self.assertEqual(vbc.read_version(self.tmp, plugin_vf), "2.0.0")
        self.assertTrue(vbc.write_version(self.tmp, plugin_vf, "2.1.0"))
        manifest = json.loads((self.tmp / "manifest.json").read_text())
        self.assertEqual(manifest["plugins"][1]["version"], "2.1.0")
        self.assertTrue((self.tmp / "manifest.json").read_text().endswith("\n"))

        nested_vf = vbc.VersionFile("manifest.json", "json_path", "metadata.tool.version")
        self.assertEqual(vbc.read_version(self.tmp, nested_vf), "3.0.0")

        self.assertIsNone(vbc._json_walk_get(manifest, ""))
        self.assertIsNone(vbc._json_walk_get(manifest, "plugins..version"))
        self.assertIsNone(vbc._json_walk_get(manifest, "metadata.missing.version"))
        self.assertIsNone(vbc._json_walk_get(manifest, "plugins.nope.version"))
        self.assertIsNone(vbc._json_walk_get(manifest, "plugins.4.version"))
        self.assertIsNone(vbc._json_walk_get(manifest, "metadata.tool.version.patch"))

        self.assertFalse(vbc._json_walk_set(manifest, "", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "plugins..version", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "plugins.nope.version", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "plugins.4.version", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "metadata.tool.version.patch", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "plugins.0.missing.0", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "plugins.0.version.patch", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "plugins.0.version.patch.leaf", "x"))

        root_list = [{"version": "1.0.0"}]
        self.assertTrue(vbc._json_walk_set(root_list, "0", {"version": "1.1.0"}))
        self.assertEqual(root_list[0]["version"], "1.1.0")
        self.assertFalse(vbc._json_walk_set(root_list, "nope", "x"))
        self.assertFalse(vbc._json_walk_set(root_list, "4", "x"))

    def test_version_file_failure_modes_are_fail_closed(self) -> None:
        vbc = self._import_gate_module("version_bump_check")
        vbs = self._import_gate_module("version_bump_surfaces")

        self.assertIsNone(
            vbc.read_version(self.tmp, vbc.VersionFile("missing.json", "json_field", "version"))
        )
        with mock.patch.object(vbs, "_vbc", return_value=object()):
            self.assertIs(vbs._h("already_bumped"), vbs.already_bumped)
        self.assertIsNone(
            vbc._extract_version_from_text(
                "{not json}\n",
                vbc.VersionFile("manifest.json", "json_path", "plugins.0.version"),
            )
        )
        self.assertEqual(
            vbc._extract_version_from_text(
                '[project]\nversion = "4.5.6"\n',
                vbc.VersionFile("pyproject.toml", "pyproject_version"),
            ),
            "4.5.6",
        )
        self.assertEqual(
            vbc._extract_version_from_text(
                '__version__ = "7.8.9"\n',
                vbc.VersionFile("pkg/__init__.py", "python_dunder_version"),
            ),
            "7.8.9",
        )
        self.assertIsNone(
            vbc._extract_version_from_text(
                "version=1.0.0\n",
                vbc.VersionFile("VERSION.txt", "regex",
                                pattern=r"version=(\d+\.\d+\.\d+)"),
            )
        )

        self.f.write("bad-field.json", "{not json}\n")
        self.f.write("bad-path.json", "{not json}\n")
        self.f.write("empty-path.json", '{"plugins": []}\n')
        self.f.write("not-project.cmake", "cmake_minimum_required(VERSION 3.25)\n")
        self.f.write("plain.txt", "no version here\n")

        self.assertIsNone(
            vbc.read_version(
                self.tmp,
                vbc.VersionFile("not-project.cmake", "cmake_project_version"),
            )
        )
        self.assertIsNone(
            vbc.read_version(self.tmp, vbc.VersionFile("bad-path.json", "json_path", "a.b"))
        )
        self.assertIsNone(
            vbc.read_version(
                self.tmp,
                vbc.VersionFile("plain.txt", "unknown_kind"),
            )
        )
        self.assertFalse(
            vbc.write_version(
                self.tmp,
                vbc.VersionFile("bad-field.json", "json_field", "version"),
                "1.0.0",
            )
        )
        self.assertFalse(
            vbc.write_version(
                self.tmp,
                vbc.VersionFile("bad-path.json", "json_path", "a.b"),
                "1.0.0",
            )
        )
        self.assertFalse(
            vbc.write_version(
                self.tmp,
                vbc.VersionFile("empty-path.json", "json_path", "plugins.4.version"),
                "1.0.0",
            )
        )
        self.assertFalse(
            vbc.write_version(
                self.tmp,
                vbc.VersionFile("plain.txt", "regex", pattern=r"missing=(\d+\.\d+\.\d+)"),
                "1.0.0",
            )
        )
        self.assertFalse(
            vbc.write_version(
                self.tmp,
                vbc.VersionFile("plain.txt", "unknown_kind"),
                "1.0.0",
            )
        )

    def test_base_version_helpers_cover_missing_and_equal_paths(self) -> None:
        vbc = self._import_gate_module("version_bump_check")
        vbs = self._import_gate_module("version_bump_surfaces")

        vf = vbc.VersionFile("CMakeLists.txt", "cmake_project_version")
        with mock.patch.object(vbc, "version_at_base", return_value="0.1.0"):
            self.assertFalse(vbc.already_bumped("origin/main", vf, self.tmp))
        with mock.patch.object(
            vbs.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                ["git"], 0, stdout="project(Pulp VERSION 9.8.7)\n",
            ),
        ):
            self.assertEqual(vbc.version_at_base("origin/main", vf), "9.8.7")
        self.assertIsNone(
            vbc.version_at_base(
                "origin/main",
                vbc.VersionFile("missing.json", "json_field", "version"),
            )
        )

        self.assertTrue(vbc.write_version(self.tmp, vf, "0.2.0"))
        with mock.patch.object(vbc, "version_at_base", return_value="0.1.0"):
            self.assertTrue(vbc.already_bumped("origin/main", vf, self.tmp))
        with mock.patch.object(vbc, "version_at_base", return_value="0.2.0"):
            self.assertFalse(vbc.already_bumped("origin/main", vf, self.tmp))
        with mock.patch.object(vbc, "version_at_base", return_value=None):
            self.assertFalse(vbc.already_bumped("origin/main", vf, self.tmp))
        with mock.patch.object(vbc, "version_at_base", return_value="1.0.0"):
            self.assertFalse(
                vbc.already_bumped(
                    "origin/main",
                    vbc.VersionFile("missing.txt", "regex",
                                    pattern=r"version=(\d+\.\d+\.\d+)"),
                    self.tmp,
                )
            )

        self.f.write("broken.json", "{not json}\n")
        with mock.patch.object(vbc, "version_at_base", return_value="1.0.0"):
            self.assertFalse(
                vbc.already_bumped(
                    "origin/main",
                    vbc.VersionFile("broken.json", "json_field", "version"),
                    self.tmp,
                )
            )

    def test_version_config_loader_strips_metadata(self) -> None:
        vbc = self._import_gate_module("version_bump_check")

        cfg_path = self.tmp / "custom-versioning.json"
        cfg_path.write_text(json.dumps({
            "$schema": "https://example.invalid/schema.json",
            "_comment": "ignored top-level metadata",
            "generated_globs": ["build/**"],
            "trailers": {"version_bump": "Custom-Bump"},
            "surfaces": {
                "sdk": {
                    "_comment": "ignored surface metadata",
                    "label": "SDK",
                    "version_files": [{
                        "_note": "ignored version-file metadata",
                        "path": "CMakeLists.txt",
                        "kind": "cmake_project_version",
                    }],
                    "trigger_paths": ["core/**"],
                    "public_api_paths": ["core/**/include/**"],
                    "internal_only_paths": ["core/**/src/**"],
                    "changelog": "CHANGELOG.md",
                },
            },
        }, indent=2) + "\n")

        cfg = vbc.load_config(cfg_path)
        self.assertEqual(cfg.generated_globs, ["build/**"])
        self.assertEqual(cfg.trailer_version_bump, "Custom-Bump")
        self.assertEqual(len(cfg.surfaces), 1)
        self.assertEqual(cfg.surfaces[0].name, "sdk")
        self.assertEqual(cfg.surfaces[0].version_files[0].path, "CMakeLists.txt")
        self.assertEqual(cfg.surfaces[0].changelog, "CHANGELOG.md")

    def test_inspector_is_an_sdk_release_surface(self) -> None:
        # Inspector libraries and headers are installed with the SDK. Keep the
        # public headers (including installed .inc protocol definitions), CMake
        # target contracts, and install/export wiring on the minor lane. Keep
        # implementation changes on the patch lane.
        vbc = self._import_gate_module("version_bump_check")
        cfg_path = VBC.parents[2] / "tools/scripts/versioning.json"
        cfg = json.loads(cfg_path.read_text())
        sdk = cfg["surfaces"]["sdk"]

        self.assertIn("inspect/include/**", sdk["trigger_paths"])
        self.assertIn("inspect/src/**", sdk["trigger_paths"])
        self.assertIn("core/**/js/**", sdk["trigger_paths"])
        self.assertIn("core/**/js/**", sdk["internal_only_paths"])
        self.assertIn("inspect/CMakeLists.txt", sdk["trigger_paths"])
        self.assertIn(
            "tools/cmake/PulpInstallRules.cmake", sdk["trigger_paths"]
        )
        self.assertIn("tools/cmake/PulpAuv3.cmake", sdk["trigger_paths"])
        self.assertIn("inspect/include/**", sdk["public_api_paths"])
        self.assertIn("inspect/CMakeLists.txt", sdk["public_api_paths"])
        self.assertIn(
            "tools/cmake/PulpInstallRules.cmake", sdk["public_api_paths"]
        )
        self.assertIn("tools/cmake/PulpAuv3.cmake", sdk["public_api_paths"])
        self.assertIn("inspect/src/**", sdk["internal_only_paths"])

        surface = next(s for s in vbc.load_config(cfg_path).surfaces
                       if s.name == "sdk")
        with mock.patch.object(
            vbc, "git_diff_ignore_whitespace_nonempty", return_value=True
        ):
            expected_levels = {
                "inspect/include/pulp/inspect/session.hpp": "minor",
                "inspect/include/pulp/inspect/protocol_methods.inc": "minor",
                "inspect/src/session.cpp": "patch",
                "inspect/CMakeLists.txt": "minor",
                "tools/cmake/PulpInstallRules.cmake": "minor",
                "tools/cmake/PulpAuv3.cmake": "minor",
            }
            for path, expected in expected_levels.items():
                with self.subTest(path=path):
                    self.assertEqual(
                        vbc.heuristic_for_surface(
                            surface, [path], "base", "head"
                        ),
                        expected,
                    )

    def test_release_product_matrix_is_an_sdk_release_surface(self) -> None:
        cfg_path = VBC.parents[2] / "tools/scripts/versioning.json"
        cfg = json.loads(cfg_path.read_text())

        self.assertIn(
            "tools/scripts/release_product_matrix.json",
            cfg["surfaces"]["sdk"]["trigger_paths"],
        )

    def test_sdk_provenance_contract_is_an_sdk_release_surface(self) -> None:
        # The CMake guard is installed in every SDK, while the Python helper
        # authors the release provenance marker. Changes to either must be
        # assigned to an SDK release instead of disappearing as tooling-only.
        vbc = self._import_gate_module("version_bump_check")
        cfg_path = VBC.parents[2] / "tools/scripts/versioning.json"
        cfg = json.loads(cfg_path.read_text())
        sdk = cfg["surfaces"]["sdk"]

        self.assertIn(
            "tools/cmake/PulpSdkProvenance.cmake", sdk["trigger_paths"]
        )
        self.assertIn("tools/scripts/sdk_provenance.py", sdk["trigger_paths"])
        self.assertIn(
            "tools/cmake/PulpSdkProvenance.cmake", sdk["public_api_paths"]
        )

        surface = next(s for s in vbc.load_config(cfg_path).surfaces
                       if s.name == "sdk")
        with mock.patch.object(
            vbc, "git_diff_ignore_whitespace_nonempty", return_value=True
        ):
            expected_levels = {
                "tools/cmake/PulpSdkProvenance.cmake": "minor",
                "tools/scripts/sdk_provenance.py": "patch",
            }
            for path, expected in expected_levels.items():
                with self.subTest(path=path):
                    self.assertEqual(
                        vbc.heuristic_for_surface(
                            surface, [path], "base", "head"
                        ),
                        expected,
                    )

    def test_rack_toolchain_is_an_sdk_release_surface(self) -> None:
        # Forge packages these tools from the source commit recorded by its
        # exact official Pulp SDK. A toolchain fix therefore needs a new SDK
        # source identity even though the scripts are not installed in the
        # binary SDK prefix itself.
        vbc = self._import_gate_module("version_bump_check")
        cfg_path = VBC.parents[2] / "tools/scripts/versioning.json"
        cfg = json.loads(cfg_path.read_text())

        self.assertIn(
            "tools/rack/**",
            cfg["surfaces"]["sdk"]["trigger_paths"],
        )
        surface = next(s for s in vbc.load_config(cfg_path).surfaces
                       if s.name == "sdk")
        with mock.patch.object(
            vbc, "git_diff_ignore_whitespace_nonempty", return_value=True
        ):
            self.assertEqual(
                vbc.heuristic_for_surface(
                    surface, ["tools/rack/install_toolchain.sh"], "base", "head"
                ),
                "patch",
            )

    def test_vendored_sources_are_an_sdk_release_surface(self) -> None:
        # Vendored sources compiled into the SDK libraries, and the patches
        # carried on them, ship in the SDK even though they sit outside
        # core/. A fix to one of them must reach an SDK release on the patch
        # lane, as must a build module that pins or compiles SDK code. Skia's
        # include/ and modules/ headers are installed and included by public
        # Pulp headers, so a change there is on the minor lane; a VERSION.md
        # re-cut is patch. Fonts that are not embedded, test helpers and the
        # web-module helpers stay outside the surface.
        vbc = self._import_gate_module("version_bump_check")
        cfg_path = VBC.parents[2] / "tools/scripts/versioning.json"
        surface = next(s for s in vbc.load_config(cfg_path).surfaces
                       if s.name == "sdk")
        with mock.patch.object(
            vbc, "git_diff_ignore_whitespace_nonempty", return_value=True
        ):
            expected_levels = {
                "external/dr_libs/dr_mp3.h": "patch",
                "external/dr_libs/stb_vorbis.c": "patch",
                "external/miniz/miniz.c": "patch",
                "external/pugixml/pugixml.cpp": "patch",
                "external/nanosvg/nanosvgrast.h": "patch",
                "external/tweetnacl/tweetnacl.c": "patch",
                "external/cpp-httplib/httplib.h": "patch",
                "external/lilv/lilv/src/world.c": "patch",
                "external/fonts/Inter-Regular.ttf": "patch",
                "external/fonts/NotoColorEmoji.ttf": "patch",
                "tools/deps/patches/dr_libs/0001-dr_mp3-64-bit-xing-frame-count.patch": "patch",
                "tools/cmake/PulpPatchChoc.cmake": "patch",
                "external/skia-build/include/core/SkCanvas.h": "minor",
                "external/skia-build/modules/skcms/skcms.h": "minor",
                "external/skia-build/VERSION.md": "patch",
                "tools/cmake/PulpDependencies.cmake": "patch",
                "core/view/cmake/PulpViewJsEngines.cmake": "patch",
                "tools/cmake/PulpWgpuNative.cmake": "patch",
                "tools/cmake/PulpTestSuite.cmake": "none",
                "tools/cmake/PulpWam.cmake": "none",
                "external/fonts/FunnelDisplay-VariableFont_wght.ttf": "none",
                "external/fonts/README.md": "none",
                "external/vst3sdk/CMakeLists.txt": "none",
            }
            for path, expected in expected_levels.items():
                with self.subTest(path=path):
                    self.assertEqual(
                        vbc.heuristic_for_surface(
                            surface, [path], "base", "head"
                        ),
                        expected,
                    )

    def test_version_bump_missing_config_returns_usage_error(self) -> None:
        code, out = _run(
            [
                "python3", str(VBC),
                "--base", "origin/main",
                "--config", str(self.tmp / "missing-versioning.json"),
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 2, msg=out)
        self.assertIn("config not found", out)


REPO_ROOT = Path(__file__).resolve().parents[2]
INSTALL_RULES = "tools/cmake/PulpInstallRules.cmake"
PATCH_MANIFEST = "tools/deps/patches.json"
_INSTALL_CALL = re.compile(r"install\s*\((.*?)\n\s*\)", re.S)
_SHIPPED_MODULE = re.compile(r'/tools/cmake/(Pulp[A-Za-z0-9_]*\.cmake)"')


def installed_pulp_modules(install_rules_text: str) -> set[str]:
    """Every tools/cmake/Pulp*.cmake an install(FILES|PROGRAMS ...) ships.

    Reads each install() call whatever if() block it sits in, so a module
    installed only under an option (AAX, ARA, Android) is still counted.
    """
    shipped: set[str] = set()
    for call in _INSTALL_CALL.finditer(install_rules_text):
        body = call.group(1)
        if re.match(r"\s*(FILES|PROGRAMS)\b", body):
            shipped.update(f"tools/cmake/{name}" for name in _SHIPPED_MODULE.findall(body))
    return shipped


# Every build module in MODULE_GLOBS is classified exactly once: installed
# (read from PulpInstallRules.cmake), the install rules themselves, a
# configure-time source patch (read from tools/deps/patches.json), or one of
# the two maps below. A module nobody classified fails
# test_every_build_module_is_classified_exactly_once.
MODULE_GLOBS = ("tools/cmake/Pulp*.cmake", "core/view/cmake/*.cmake")

# sdk trigger paths that are not installed: each pins a dependency the SDK
# links or changes the code its libraries compile.
COMPILED_SDK_MODULES = {
    "core/view/cmake/PulpDesignUnits.cmake": "defines the DesignIR and importer libraries the view umbrella links",
    "core/view/cmake/PulpViewJsEngines.cmake": "selects the JS engine compiled into pulp-view",
    "core/view/cmake/PulpViewJsPreludes.cmake": "embeds the JS preludes compiled into pulp-view",
    "core/view/cmake/PulpViewPlatform.cmake": "defines the pulp-view-platform library",
    "core/view/cmake/PulpViewSources.cmake": "lists the native view sources compiled into pulp-view",
    "tools/cmake/PulpAndroid.cmake": "adds the Android platform sources and Oboe audio to the SDK libraries",
    "tools/cmake/PulpDependencies.cmake": "pins the CHOC fork, WebGPU, wgpu and resolves the Skia archive",
    "tools/cmake/PulpFloatingPointContraction.cmake": "sets the FP contraction flags every SDK library compiles with",
    "tools/cmake/PulpForgeCatalogInstall.cmake": "installs the Forge catalog export into the SDK",
    "tools/cmake/PulpGitState.cmake": "compiles the checkout's dirty state into pulp-runtime build info",
    "tools/cmake/PulpGpuAudioProviderIdentity.cmake": "pins the GPU-audio provider identity pulp-gpu-audio is built against",
    "tools/cmake/PulpGpuRuntime.cmake": "wires the gpu-runtime device layer pulp-render links",
    "tools/cmake/PulpLiteEditorObjects.cmake": "builds the lite editor replacement objects plug-ins link",
    "tools/cmake/PulpMacOsArch.cmake": "selects the macOS architectures the SDK is built for",
    "tools/cmake/PulpPortableWav.cmake": "sets the dr_libs include and defines pulp-audio compiles with",
    "tools/cmake/PulpProfiles.cmake": "holds the option set a release SDK configure uses",
    "tools/cmake/PulpRack.cmake": "builds the Rack plug-ins whose tools/rack sources are already sdk triggers",
    "tools/cmake/PulpReleaseMatrix.cmake": "compiles the release floors into the CLI",
    "tools/cmake/PulpRuntimePins.cmake": "writes and installs runtime-pins.json",
    "tools/cmake/PulpSdl3Linux.cmake": "sets the SDL3 options the Linux SDK links",
    "tools/cmake/PulpSimdSupport.cmake": "selects the SIMD target pulp-runtime compiles for",
    "tools/cmake/PulpSkiaCache.cmake": "selects the Skia cache generation the SDK links",
    "tools/cmake/PulpTracing.cmake": "defines pulp-tracing, which pulp-runtime links publicly and the SDK exports",
    "tools/cmake/PulpV8ConsumerDefines.cmake": "applies the V8 consumer defines the V8 engine compiles with",
    "tools/cmake/PulpV8Windows.cmake": "applies the Windows V8 consumer ABI contract",
    "tools/cmake/PulpWgpuNative.cmake": "pins the wgpu-native archives by digest",
    "tools/cmake/PulpWgpuUniversal.cmake": "builds the universal libwgpu_native the macOS SDK ships",
    "tools/cmake/PulpWindowsSkiaProvider.cmake": "selects the Windows ARM64EC/ARM64X Skia and Dawn provider",
}

# Build modules outside the sdk surface.
NON_SDK_MODULES = {
    "tools/cmake/PulpAbletonLink.cmake": "optional developer-supplied Link SDK boundary; never bundled",
    "tools/cmake/PulpCargoStaticlib.cmake": "opt-in Rust native-component test lane",
    "tools/cmake/PulpCatch.cmake": "Catch2 test discovery",
    "tools/cmake/PulpCatchAddTests.cmake": "Catch2 test discovery",
    "tools/cmake/PulpCheckForgeCatalog.cmake": "packaging guard comparing the installed Forge catalog",
    "tools/cmake/PulpCheckoutLocation.cmake": "refuses checkouts under a temporary directory",
    "tools/cmake/PulpCollectTargets.cmake": "lists configured targets for build tooling",
    "tools/cmake/PulpConfigureCheckCache.cmake": "caches configure-check results",
    "tools/cmake/PulpConsumerClosure.cmake": "measures what an installed SDK costs a consumer",
    "tools/cmake/PulpCppStringChunks.cmake": "string chunking for tools/mcp, which is not on the sdk surface",
    "tools/cmake/PulpDefaultBuildType.cmake": "defaults an empty build type for the top-level project only",
    "tools/cmake/PulpEmbed.cmake": "nothing in the tree includes it",
    "tools/cmake/PulpFaust.cmake": "nothing in the tree includes it; optional external Faust codegen",
    "tools/cmake/PulpFetchContent.cmake": "keeps shared FetchContent caches immutable",
    "tools/cmake/PulpGenerateForgeCatalog.cmake": "packaging guard on the Forge catalog exporter",
    "tools/cmake/PulpGpuWasm.cmake": "WebAssembly DSP module helper, outside the native SDK",
    "tools/cmake/PulpInstrumentation.cmake": "coverage flags, applied only when PULP_ENABLE_COVERAGE is ON",
    "tools/cmake/PulpLinkFloor.cmake": "link-closure measurement",
    "tools/cmake/PulpLinkMaps.cmake": "records link maps for the macOS gate",
    "tools/cmake/PulpLinkPool.cmake": "caps concurrent Ninja link jobs",
    "tools/cmake/PulpLiveKernel.cmake": "experimental live-kernel helper; nothing in the tree includes it",
    "tools/cmake/PulpSampleRegionWebSources.cmake": "web build sources for WAM and WebCLAP",
    "tools/cmake/PulpTestData.cmake": "declares compiled-test data inputs",
    "tools/cmake/PulpTestSharedObjects.cmake": "shares test-support objects",
    "tools/cmake/PulpTestSuite.cmake": "declares Catch2 test binaries",
    "tools/cmake/PulpTestTimeout.cmake": "scales test timeouts by build config",
    "tools/cmake/PulpToolchainCheck.cmake": "reports host tool version drift",
    "tools/cmake/PulpWam.cmake": "WAM WebAssembly helper, outside the native SDK",
    "tools/cmake/PulpWclap.cmake": "WebCLAP WebAssembly helper, outside the native SDK",
    "tools/cmake/PulpWebUi.cmake": "browser UI WebAssembly helper, outside the native SDK",
}


def build_modules() -> set[str]:
    return {str(path.relative_to(REPO_ROOT)) for pattern in MODULE_GLOBS
            for path in REPO_ROOT.glob(pattern)}


def classify_modules(modules: set[str], shipped: set[str]) -> dict[str, list[str]]:
    """Map each module to every bucket that claims it."""
    buckets = {
        "shipped": shipped,
        "install rules": {INSTALL_RULES},
        "source patch": source_patch_modules(),
        "compiled SDK": set(COMPILED_SDK_MODULES),
        "non-SDK": set(NON_SDK_MODULES),
    }
    return {module: [name for name, members in buckets.items() if module in members]
            for module in modules}


EMBEDDED_FONT = re.compile(r"\$\{PULP_ROOT_DIR\}/(external/fonts/[^\s)]+\.ttf)")


def embedded_fonts(canvas_cmake_text: str) -> set[str]:
    """Every external/fonts/*.ttf core/canvas/CMakeLists.txt compiles in."""
    return set(EMBEDDED_FONT.findall(canvas_cmake_text))


def source_patch_modules() -> set[str]:
    """CMake modules that rewrite fetched dependency source at configure time.

    tools/deps/patches.json names them under configure_time_patches. They are
    not installed, but the source they rewrite is compiled into the SDK, so
    they are sdk trigger paths without shipping.
    """
    manifest = json.loads((REPO_ROOT / PATCH_MANIFEST).read_text(encoding="utf-8"))
    return {dep["configure_time_patches"]["file"] for dep in manifest["dependencies"]
            if "configure_time_patches" in dep}


def sdk_module_drift(install_rules_text: str, sdk_trigger_paths: list[str]) -> tuple[set[str], set[str]]:
    """(shipped modules the sdk surface does not trigger on,
    listed tools/cmake/Pulp*.cmake modules that do not ship)."""
    shipped = installed_pulp_modules(install_rules_text)
    listed = {p for p in sdk_trigger_paths
              if re.fullmatch(r"tools/cmake/Pulp[A-Za-z0-9_]*\.cmake", p)}
    # The install rules file defines what ships rather than shipping itself,
    # and source-patch and compiled-SDK modules change SDK code without shipping.
    return shipped - listed, (listed - shipped - {INSTALL_RULES} - source_patch_modules()
                              - set(COMPILED_SDK_MODULES))


class SdkShippedCmakeModulesTests(unittest.TestCase):
    """Every CMake module the SDK installs is an sdk trigger path, so a fix to
    one cuts a release (version-at-land ignores a Version-Bump trailer on a
    surface the diff does not touch)."""

    def setUp(self) -> None:
        config = json.loads((REPO_ROOT / "tools/scripts/versioning.json").read_text(encoding="utf-8"))
        self.triggers = config["surfaces"]["sdk"]["trigger_paths"]
        self.rules = (REPO_ROOT / INSTALL_RULES).read_text(encoding="utf-8")

    def test_the_sdk_surface_lists_exactly_the_shipped_modules(self) -> None:
        unlisted, unshipped = sdk_module_drift(self.rules, self.triggers)
        self.assertEqual(unlisted, set(), "installed by PulpInstallRules.cmake but not an sdk trigger path")
        self.assertEqual(unshipped, set(), "an sdk trigger path for a module the SDK does not install")
        self.assertGreater(len(installed_pulp_modules(self.rules)), 30,
                           "the install-list parser found almost nothing; it is broken")

    def test_compiled_sdk_modules_are_unshipped_triggers(self) -> None:
        shipped = installed_pulp_modules(self.rules)
        for module in sorted(COMPILED_SDK_MODULES):
            with self.subTest(module=module):
                self.assertTrue((REPO_ROOT / module).is_file(), "listed module does not exist")
                self.assertIn(module, self.triggers, "classified as compiled SDK code but not a trigger path")
                self.assertNotIn(module, shipped, "installed, so the shipped-module check already covers it")

    def test_every_build_module_is_classified_exactly_once(self) -> None:
        modules = build_modules()
        self.assertGreater(len(modules), 100, "the module globs found almost nothing; they are broken")
        claims = classify_modules(modules, installed_pulp_modules(self.rules))
        unclassified = sorted(m for m, owners in claims.items() if not owners)
        ambiguous = {m: owners for m, owners in claims.items() if len(owners) > 1}
        self.assertEqual(unclassified, [], "build module in no bucket; add it to COMPILED_SDK_MODULES "
                         "(and the sdk trigger paths) or NON_SDK_MODULES with a reason")
        self.assertEqual(ambiguous, {}, "build module in more than one bucket")
        for name, table in (("COMPILED_SDK_MODULES", COMPILED_SDK_MODULES), ("NON_SDK_MODULES", NON_SDK_MODULES)):
            for module, reason in table.items():
                with self.subTest(table=name, module=module):
                    self.assertIn(module, modules, "classified module does not exist")
                    self.assertTrue(reason.strip(), "classification needs a reason")
        self.assertEqual(sorted(set(NON_SDK_MODULES) & set(self.triggers)), [],
                         "a non-SDK module is an sdk trigger path")

    def test_the_sdk_font_triggers_are_the_embedded_fonts(self) -> None:
        embedded = embedded_fonts((REPO_ROOT / "core/canvas/CMakeLists.txt").read_text(encoding="utf-8"))
        self.assertGreater(len(embedded), 3, "the font parser found almost nothing; it is broken")
        listed = {p for p in self.triggers if p.startswith("external/fonts/")}
        self.assertEqual(listed, embedded,
                         "sdk font triggers must be exactly the fonts core/canvas embeds")

    def test_every_source_patch_module_is_an_sdk_trigger(self) -> None:
        patchers = source_patch_modules()
        self.assertIn("tools/cmake/PulpPatchChoc.cmake", patchers,
                      "patches.json lists no configure-time patch module; the reader is broken")
        self.assertEqual(patchers - set(self.triggers), set(),
                         "rewrites SDK source at configure time but is not an sdk trigger path")

    def test_a_planted_install_or_a_dropped_trigger_is_reported(self) -> None:
        planted = self.rules + (
            '\nif(PULP_PLANTED)\ninstall(FILES\n'
            '    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpPlantedModule.cmake"\n'
            '    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp\n)\nendif()\n')
        unlisted, _ = sdk_module_drift(planted, self.triggers)
        self.assertEqual(unlisted, {"tools/cmake/PulpPlantedModule.cmake"})
        dropped = [p for p in self.triggers if p != "tools/cmake/PulpPlugin.cmake"]
        unlisted, _ = sdk_module_drift(self.rules, dropped)
        self.assertEqual(unlisted, {"tools/cmake/PulpPlugin.cmake"})
        _, unshipped = sdk_module_drift(self.rules, self.triggers + ["tools/cmake/PulpTestSuite.cmake"])
        self.assertEqual(unshipped, {"tools/cmake/PulpTestSuite.cmake"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
