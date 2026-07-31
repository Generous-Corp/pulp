#!/usr/bin/env python3
"""Positive and negative controls for win32_include_lint.py."""
from __future__ import annotations

import importlib.util
import pathlib
import tempfile
import unittest

MODULE_PATH = pathlib.Path(__file__).with_name("win32_include_lint.py")
SPEC = importlib.util.spec_from_file_location("win32_include_lint", MODULE_PATH)
lint = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lint)

RAW = "#pragma once\n#if defined(_WIN32)\n#include <windows.h>\n#endif\n"
SANE = "#pragma once\n#if defined(_WIN32)\n#include <pulp/platform/win32_sane.hpp>\n#endif\n"
GUARDED = "#pragma once\n#define NOMINMAX\n#include <windows.h>\n"
SKIPPED = (
    "#pragma once\n#include <windows.h>  // win32-include-lint: skip vendored shim\n"
)


class Win32IncludeLintTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")

    def _run(self) -> int:
        return lint.main(["--root", str(self.root)])

    # --- the defect this lint exists for -------------------------------------
    def test_raw_windows_include_in_public_header_fails(self):
        self._write("core/view/include/pulp/view/widget_bridge.hpp", RAW)
        self.assertEqual(self._run(), 1)

    def test_message_names_the_file_and_line(self):
        self._write("core/view/include/pulp/view/widget_bridge.hpp", RAW)
        msgs = lint.scan(
            self.root / "core/view/include/pulp/view/widget_bridge.hpp",
            "core/view/include/pulp/view/widget_bridge.hpp",
        )
        self.assertEqual(len(msgs), 1)
        self.assertIn("widget_bridge.hpp:3", msgs[0])
        self.assertIn("win32_sane.hpp", msgs[0])

    # --- things that must NOT fail (or the lint is unusable) ------------------
    def test_win32_sane_include_passes(self):
        self._write("core/view/include/pulp/view/widget_bridge.hpp", SANE)
        self.assertEqual(self._run(), 0)

    def test_nominmax_before_the_include_passes(self):
        # reload_library.hpp / dl_shim.hpp legitimately do this.
        self._write("core/host/include/pulp/host/dl_shim.hpp", GUARDED)
        self.assertEqual(self._run(), 0)

    def test_the_canonical_wrapper_may_include_windows_raw(self):
        self._write("core/platform/include/pulp/platform/win32_sane.hpp", RAW)
        self.assertEqual(self._run(), 0)

    def test_inline_skip_comment_is_honoured(self):
        self._write("core/view/include/pulp/view/shim.hpp", SKIPPED)
        self.assertEqual(self._run(), 0)

    def test_header_without_windows_include_passes(self):
        self._write("core/view/include/pulp/view/widgets.hpp", "#pragma once\n")
        self.assertEqual(self._run(), 0)

    def test_sources_are_out_of_scope(self):
        # A .cpp breaks only its own TU; the lint guards the installed surface.
        self._write("core/view/src/widget_bridge.cpp", RAW)
        self.assertEqual(self._run(), 0)

    def test_clean_tree_passes(self):
        self._write("core/view/include/pulp/view/widgets.hpp", SANE)
        self._write("core/host/include/pulp/host/dl_shim.hpp", GUARDED)
        self.assertEqual(self._run(), 0)


if __name__ == "__main__":
    unittest.main()
