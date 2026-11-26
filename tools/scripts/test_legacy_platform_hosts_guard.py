#!/usr/bin/env python3
"""Positive and negative controls for legacy_platform_hosts_guard.py.

Every planted include, path and link into the quarantine must turn the guard
red, look-alike platform paths outside it must stay green, the real repository
must pass with its allowlist and fail without it, and a tree with nothing to
scan must be a scan error rather than a clean tree.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
SPEC = importlib.util.spec_from_file_location("legacy_platform_hosts_guard",
                                              HERE / "legacy_platform_hosts_guard.py")
guard = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = guard
SPEC.loader.exec_module(guard)

REPO = HERE.parents[1]

# One member per platform, at either location, makes a scannable tree.
BASE_TREE = {
    "CMakeLists.txt": "add_subdirectory(core/view)\n",
    "core/view/CMakeLists.txt": "add_library(pulp-view STATIC src/view.cpp)\n",
    "core/view/src/view.cpp": "#include <pulp/view/view.hpp>\n",
    "core/view/platform/win/plugin_view_host_win.cpp":
        "#include <pulp/view/platform/win_pointer_input.hpp>\n",
    "core/view/include/pulp/view/platform/win_pointer_input.hpp": "#pragma once\n",
    "integrations/legacy-platform-hosts/linux/src/accessibility_linux.cpp":
        "#include <pulp/view/platform/atspi_mapping.hpp>\n",
    "integrations/legacy-platform-hosts/linux/include/pulp/view/platform/atspi_mapping.hpp":
        "#pragma once\n",
    "core/view/platform/ios/window_host_ios.mm": "#import <UIKit/UIKit.h>\n",
    "core/render/platform/android/gpu_surface_android.cpp": "int android_surface;\n",
    "core/view/platform/web/window_host_web.cpp":
        "#include <pulp/view/web/web_event_translate.hpp>\n",
    "test/test_uia_mapping.cpp": "#include <pulp/view/platform/uia_mapping.hpp>\n",
}


class LegacyPlatformHostsGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        for rel, text in BASE_TREE.items():
            self._write(rel, text)

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")

    def _check(self, allowed=None, root: pathlib.Path | None = None) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            rc = guard.check(root or self.root, {} if allowed is None else allowed)
        return rc, out.getvalue()

    def assertRed(self, *, kind: str, platform: str):
        rc, text = self._check()
        self.assertEqual(rc, 1, text)
        self.assertIn(f"{kind} reference(s) to the {platform} quarantine", text)

    # -- clean trees -------------------------------------------------------

    def test_clean_synthetic_tree_passes(self):
        rc, text = self._check()
        self.assertEqual(rc, 0, text)

    def test_quarantine_may_reference_itself(self):
        self._write("integrations/legacy-platform-hosts/CMakeLists.txt",
                    "add_library(pulp-legacy-win STATIC win/src/plugin_view_host_win.cpp)\n"
                    "target_include_directories(pulp-legacy-win PRIVATE win/include)\n")
        self._write("integrations/legacy-platform-hosts/tests/test_x.cpp",
                    '#include "../win/src/plugin_view_host_win.cpp"\n')
        rc, text = self._check()
        self.assertEqual(rc, 0, text)

    def test_look_alike_platform_paths_are_not_quarantine(self):
        self._write("ship/CMakeLists.txt",
                    "add_library(pulp-ship platform/win/codesign_win.cpp\n"
                    "  ${SHIP_ROOT}/platform/win/installer_win.cpp)\n"
                    "file(GLOB X ${SHIP_ROOT}/platform/win/*.cpp)\n")
        self._write("ship/platform/win/codesign_win.cpp", "int x;\n")
        self._write("core/audio/src/io.cpp",
                    '#include "../platform/linux/alsa_device.hpp"\n'
                    "#include <pulp/platform/android/jni.hpp>\n"
                    "#include <pulp/platform/win/text_convert.hpp>\n"
                    "#include <pulp/view/platform/ns_role_mapping.hpp>\n")
        self._write("core/audio/platform/linux/alsa_device.hpp", "#pragma once\n")
        self._write("tools/cmake/PulpWebUi.cmake",
                    "list(APPEND S \"${_ROOT}/core/platform/platform/web/clipboard_web.cpp\")\n")
        rc, text = self._check()
        self.assertEqual(rc, 0, text)

    def test_comments_are_not_references(self):
        self._write("core/view/CMakeLists.txt",
                    "# platform/win/plugin_view_host_win.cpp moved to the quarantine\n"
                    "#[[ add_subdirectory(../../integrations/legacy-platform-hosts) ]]\n"
                    "add_library(pulp-view STATIC src/view.cpp)\n")
        self._write("core/view/src/view.cpp",
                    "// #include <pulp/view/platform/win_pointer_input.hpp>\n"
                    "/* #include <pulp/view/web/web_event_translate.hpp> */\n")
        rc, text = self._check()
        self.assertEqual(rc, 0, text)

    # -- planted violations ------------------------------------------------

    def test_planted_public_include_fails(self):
        self._write("core/view/src/view.cpp",
                    "#include <pulp/view/platform/win_pointer_input.hpp>\n")
        self.assertRed(kind="include", platform="win")

    def test_planted_include_of_moved_header_fails(self):
        self._write("core/view/src/view.cpp",
                    '#include "pulp/view/platform/atspi_mapping.hpp"\n')
        self.assertRed(kind="include", platform="linux")

    def test_planted_relative_include_fails(self):
        self._write("test/test_view.cpp",
                    '#include "../core/render/platform/android/gpu_surface_android.cpp"\n')
        self.assertRed(kind="include", platform="android")

    def test_planted_include_naming_the_quarantine_fails(self):
        self._write("core/view/src/view.cpp",
                    '#include "integrations/legacy-platform-hosts/web/src/web_input.cpp"\n')
        self.assertRed(kind="include", platform="web")

    def test_planted_cmake_relative_path_fails(self):
        self._write("core/view/CMakeLists.txt",
                    "add_library(pulp-view STATIC src/view.cpp platform/ios/window_host_ios.mm)\n")
        self.assertRed(kind="path", platform="ios")

    def test_planted_cmake_path_under_unknown_variable_fails(self):
        self._write("tools/cmake/PulpWebUi.cmake",
                    "file(GLOB S \"${_PULP_WEBUI_ROOT}/core/view/platform/web/*.cpp\")\n")
        self.assertRed(kind="path", platform="web")

    def test_planted_bare_quarantine_file_name_fails(self):
        self._write("test/cmake/view_tests.cmake",
                    "pulp_add_test_suite(pulp-test-uia SOURCES test_uia_mapping.cpp)\n")
        self.assertRed(kind="path", platform="tests")

    def test_planted_add_subdirectory_fails(self):
        self._write("integrations/legacy-platform-hosts/README.md", "porting reference\n")
        self._write("integrations/CMakeLists.txt", "add_subdirectory(legacy-platform-hosts)\n")
        self.assertRed(kind="path", platform="root")

    def test_planted_link_to_a_quarantine_target_fails(self):
        self._write("integrations/legacy-platform-hosts/win/CMakeLists.txt",
                    "add_library(pulp-legacy-win-host STATIC src/plugin_view_host_win.cpp)\n")
        self._write("core/format/CMakeLists.txt",
                    "add_library(pulp-format STATIC src/vst3.cpp)\n"
                    "target_link_libraries(pulp-format PRIVATE pulp-legacy-win-host)\n")
        self.assertRed(kind="link", platform="win")

    # -- allowlist ---------------------------------------------------------

    def test_allowlist_counts_are_exact(self):
        self._write("core/view/CMakeLists.txt",
                    "add_library(pulp-view STATIC src/view.cpp\n"
                    "  platform/ios/window_host_ios.mm)\n")
        key = ("core/view/CMakeLists.txt", "ios", "path")
        self.assertEqual(self._check({key: (1, "edge")})[0], 0)
        rc, text = self._check({key: (2, "edge")})
        self.assertEqual(rc, 1, text)
        self.assertIn("shrink or remove", text)
        self._write("core/view/CMakeLists.txt",
                    "add_library(pulp-view STATIC src/view.cpp\n"
                    "  platform/ios/window_host_ios.mm platform/ios/window_host_ios.mm)\n")
        rc, text = self._check({key: (1, "edge")})
        self.assertEqual(rc, 1, text)

    def test_stale_allowlist_entry_fails(self):
        rc, text = self._check({("core/view/CMakeLists.txt", "win", "path"): (1, "gone")})
        self.assertEqual(rc, 1, text)
        self.assertIn("occur 0 time(s), not 1", text)

    # -- scan errors -------------------------------------------------------

    def test_empty_tree_is_a_scan_error(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertEqual(self._check(root=pathlib.Path(empty))[0], 2)

    def test_missing_platform_members_are_a_scan_error(self):
        (self.root / "core/view/platform/web/window_host_web.cpp").unlink()
        rc, text = self._check()
        self.assertEqual(rc, 2, text)
        self.assertIn("no quarantine member for web", text)

    # -- the real repository -----------------------------------------------

    def test_repository_passes_with_its_allowlist(self):
        rc, text = self._check(guard.ALLOWED, root=REPO)
        self.assertEqual(rc, 0, text)

    def test_repository_fails_without_its_allowlist(self):
        rc, text = self._check({}, root=REPO)
        self.assertEqual(rc, 1, text)

    def test_every_allowlist_row_has_a_reason(self):
        for key, (count, reason) in guard.ALLOWED.items():
            self.assertGreater(count, 0, key)
            self.assertTrue(reason.strip(), key)
            self.assertIn(key[1], guard.MEMBERS, key)
            self.assertIn(key[2], {"include", "path", "link"}, key)


if __name__ == "__main__":
    unittest.main()
