#!/usr/bin/env python3
"""Tests for inspect_control_include_check.py.

The negative controls are the point: a scan that found nothing in a tree that
holds a violation proves nothing, so each case plants the violation and requires
it to be reported, and the baseline must pass.
"""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import inspect_control_include_check as chk  # noqa: E402

VIEW_SIDE = frozenset({"src/overlay.cpp"})


def write(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class IncludeScan(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        write(self.root, "include/pulp/inspect/broker.hpp",
              "#include <pulp/dataplane/value_channel_set.hpp>\n#include <pulp/runtime/log.hpp>\n")
        write(self.root, "src/broker.cpp", '#include <pulp/inspect/broker.hpp>\n')
        write(self.root, "src/overlay.cpp", "#include <pulp/view/view.hpp>\n")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def problems(self) -> list[str]:
        return chk.check(self.root, VIEW_SIDE)

    def test_baseline_passes(self) -> None:
        self.assertEqual(self.problems(), [])

    def test_a_view_include_in_the_control_stack_is_reported(self) -> None:
        write(self.root, "src/broker.cpp", "#include <pulp/view/value_channel_set.hpp>\n")
        self.assertEqual(self.problems(), ["control stack: src/broker.cpp includes pulp/view"])

    def test_canvas_and_render_includes_are_reported(self) -> None:
        write(self.root, "include/pulp/inspect/broker.hpp",
              '#include "pulp/canvas/canvas.hpp"\n#import <pulp/render/gpu_pass.hpp>\n')
        self.assertEqual(self.problems(),
                         ["control stack: include/pulp/inspect/broker.hpp includes "
                          "pulp/canvas, pulp/render"])

    def test_a_view_side_file_with_no_ui_include_is_stale(self) -> None:
        write(self.root, "src/overlay.cpp", "#include <pulp/runtime/log.hpp>\n")
        self.assertEqual(self.problems(),
                         ["stale: src/overlay.cpp is listed as view side but includes no UI header"])

    def test_a_missing_view_side_file_is_stale(self) -> None:
        (self.root / "src/overlay.cpp").unlink()
        self.assertEqual(self.problems(),
                         ["stale: src/overlay.cpp is listed as view side but does not exist"])

    def test_a_commented_include_is_not_an_include(self) -> None:
        write(self.root, "src/broker.cpp", "// #include <pulp/view/view.hpp>\n")
        self.assertEqual(self.problems(), [])

    def test_a_look_alike_module_is_not_a_ui_root(self) -> None:
        write(self.root, "src/broker.cpp", "#include <pulp/viewport/x.hpp>\n#include <pulp/renderer/y.hpp>\n")
        self.assertEqual(self.problems(), [])

    def test_main_reports_exit_codes(self) -> None:
        out, err = io.StringIO(), io.StringIO()
        write(self.root, "src/broker.cpp", "#include <pulp/view/view.hpp>\n")
        with redirect_stdout(out), redirect_stderr(err):
            code = chk.main(["--inspect-dir", str(self.root)])
        self.assertEqual(code, 1)
        self.assertIn("FAIL", err.getvalue())
        with redirect_stdout(out), redirect_stderr(err):
            self.assertEqual(chk.main(["--inspect-dir", str(self.root / "nowhere")]), 2)

    def test_the_real_view_side_list_names_existing_files(self) -> None:
        inspect_dir = Path(__file__).resolve().parents[2] / "inspect"
        if not inspect_dir.is_dir():
            self.skipTest("no inspect/ directory beside this script")
        self.assertEqual(chk.check(inspect_dir), [])


if __name__ == "__main__":
    unittest.main()
