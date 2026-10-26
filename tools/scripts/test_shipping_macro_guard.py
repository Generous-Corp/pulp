#!/usr/bin/env python3
"""Positive and negative controls for shipping_macro_guard.py."""
from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import tempfile
import unittest

MODULE_PATH = pathlib.Path(__file__).with_name("shipping_macro_guard.py")
SPEC = importlib.util.spec_from_file_location("shipping_macro_guard", MODULE_PATH)
guard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guard)

REPO = pathlib.Path(__file__).resolve().parents[2]
HOOKED = "core/format/src/au_v2_cocoa_view.mm"
GUARDED = ("#if defined(PULP_AU_V2_TEST_HOOKS)\n"
           "+ (void*)pulpEditorHostForTesting:(NSView*)view { return nullptr; }\n"
           "#endif\n")


class ShippingMacroGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        self._write(HOOKED, "// The test target defines PULP_AU_V2_TEST_HOOKS.\n" + GUARDED)
        self._write("test/cmake/format_reload_tests.cmake",
                    "target_compile_definitions(t PRIVATE PULP_AU_V2_TEST_HOOKS=1)\n")

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")

    def _run(self, root: pathlib.Path | None = None) -> int:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return guard.main(["--root", str(root or self.root)])

    def test_guarded_source_and_test_definition_pass(self):
        self.assertEqual(self._run(), 0)

    def test_the_repository_passes(self):
        self.assertEqual(self._run(REPO), 0)

    def test_a_define_in_a_shipped_source_fails(self):
        self._write("core/format/src/au_v2_common.cpp", "#define PULP_AU_V2_TEST_HOOKS 1\n")
        self.assertEqual(self._run(), 1)

    def test_a_spaced_define_is_not_taken_for_a_comment(self):
        self._write("core/format/src/au_v2_common.cpp", "#  define PULP_AU_V2_TEST_HOOKS 1\n")
        self.assertEqual(self._run(), 1)

    def test_a_shipping_cmake_definition_fails(self):
        self._write("tools/cmake/PulpPluginFormats.cmake",
                    "target_compile_definitions(${target}_AU PRIVATE\n"
                    "    PULP_AU_GUI=1\n    PULP_AU_V2_TEST_HOOKS=1)\n")
        self.assertEqual(self._run(), 1)

    def test_a_compiler_flag_in_a_workflow_fails(self):
        self._write(".github/workflows/build.yml", "run: cmake -DPULP_AU_V2_TEST_HOOKS=1 ..\n")
        self.assertEqual(self._run(), 1)

    def test_comments_naming_the_macro_pass(self):
        self._write("tools/cmake/PulpPluginFormats.cmake",
                    "# Never set PULP_AU_V2_TEST_HOOKS here: it is test-only.\n")
        self._write("core/format/src/au_v2_common.cpp",
                    "// PULP_AU_V2_TEST_HOOKS is defined by the test target only.\n")
        self.assertEqual(self._run(), 0)

    def test_a_guard_line_carrying_more_than_the_macro_fails(self):
        self._write("core/format/src/au_v2_common.cpp",
                    "#if defined(PULP_AU_V2_TEST_HOOKS) || 1\n#endif\n")
        self.assertEqual(self._run(), 1)

    def test_code_after_a_closed_comment_fails(self):
        self._write("core/format/src/au_v2_common.cpp",
                    "/* note */ #define PULP_AU_V2_TEST_HOOKS 1\n")
        self.assertEqual(self._run(), 1)

    def test_guard_lines_with_trailing_comments_pass(self):
        self._write("core/format/src/au_v2_common.mm",
                    "#ifdef PULP_AU_V2_TEST_HOOKS // test-only\n"
                    "#endif /* PULP_AU_V2_TEST_HOOKS */\n")
        self.assertEqual(self._run(), 0)

    def test_nested_test_and_docs_directories_are_scanned(self):
        self._write("examples/forge-modular/test/hooks.cmake",
                    "add_compile_definitions(PULP_AU_V2_TEST_HOOKS=1)\n")
        self.assertEqual(self._run(), 1)
        (self.root / "examples/forge-modular/test/hooks.cmake").unlink()
        self._write("tools/figma-plugin/docs/build.sh", "cc -DPULP_AU_V2_TEST_HOOKS=1 x.c\n")
        self.assertEqual(self._run(), 1)

    def test_a_missing_guard_is_a_scan_error(self):
        self._write(HOOKED, "+ (void*)pulpEditorHostForTesting:(NSView*)view { return nullptr; }\n")
        self.assertEqual(self._run(), 2)


if __name__ == "__main__":
    unittest.main()
