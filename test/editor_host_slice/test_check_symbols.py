#!/usr/bin/env python3
"""The Vellum-side symbol checks in check_symbols.py, on planted symbol lists.

Each check must hold for a correct library in either mirror spelling and fail
for the defect it exists to catch, so none of them can pass by matching
nothing.
"""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_symbols as cs  # noqa: E402


def results(spelling: str, undefined: list[str], exported: list[str]) -> dict[str, bool]:
    return {what.split(" (first:")[0]: ok
            for ok, what in cs.vellum_ui_checks(spelling, undefined, exported)}


BOUNDARY = ["slice_vellum_ui::KnobPanel::create(unsigned int, unsigned int, float, "
            "slice_vellum_ui::PanelCallbacks)"]


class ReferencesTheSdksMirror(unittest.TestCase):
    def test_a_vellum_sdk_library_that_references_vellum_view_passes(self):
        got = results("vellum", ["vellum::view::View::layout_children()",
                                 "typeinfo for vellum::view::View"], BOUNDARY)
        self.assertTrue(all(got.values()), got)

    def test_a_pulp_sdk_library_that_references_pulp_view_passes(self):
        got = results("pulp", ["pulp::view::View::layout_children()",
                               "vtable for pulp::canvas::Canvas"], BOUNDARY)
        self.assertTrue(all(got.values()), got)

    def test_a_vellum_sdk_library_that_reached_pulps_own_view_fails_both_ways(self):
        got = results("vellum", ["pulp::view::View::layout_children()"], BOUNDARY)
        self.assertFalse(got["vellum-ui: control - it references mirrored vellum::view names"])
        self.assertFalse(got["vellum-ui: references no pulp::view/canvas/render name"])

    def test_a_vellum_sdk_library_with_a_stray_pulp_reference_fails(self):
        got = results("vellum", ["vellum::view::View::layout_children()",
                                 "pulp::canvas::Canvas::fill_rect(float, float, float, float)"],
                      BOUNDARY)
        self.assertTrue(got["vellum-ui: control - it references mirrored vellum::view names"])
        self.assertFalse(got["vellum-ui: references no pulp::view/canvas/render name"])

    def test_a_pulp_sdk_library_that_references_only_vellum_names_fails_its_control(self):
        got = results("pulp", ["vellum::view::View::layout_children()"], BOUNDARY)
        self.assertFalse(got["vellum-ui: control - it references mirrored pulp::view names"])

    def test_a_library_that_references_no_mirror_at_all_fails_its_control(self):
        for spelling in cs.MIRROR_SPELLINGS:
            got = results(spelling, ["std::terminate()"], BOUNDARY)
            self.assertFalse(got[f"vellum-ui: control - it references mirrored {spelling}::view names"])


class ExportsNoMirroredSymbol(unittest.TestCase):
    EXPORT_CHECK = "vellum-ui: exports no mirrored UI symbol in either spelling"

    def test_an_export_in_either_spelling_fails_under_either_sdk(self):
        for sdk in cs.MIRROR_SPELLINGS:
            for leaked in ("vtable for vellum::view::Knob", "pulp::canvas::Color::rgba8(int, int, int)"):
                with self.subTest(sdk=sdk, leaked=leaked):
                    got = results(sdk, [f"{sdk}::view::View::layout_children()"], BOUNDARY + [leaked])
                    self.assertFalse(got[self.EXPORT_CHECK])

    def test_the_librarys_own_boundary_is_not_a_mirrored_export(self):
        got = results("vellum", ["vellum::view::View::layout_children()"], BOUNDARY)
        self.assertTrue(got[self.EXPORT_CHECK])


class MirrorSpelling(unittest.TestCase):
    def make_sdk(self, root: Path, *spellings: str) -> Path:
        for spelling in spellings:
            header = root / "mirror/include" / spelling / "canvas/canvas.hpp"
            header.parent.mkdir(parents=True)
            header.write_text("#pragma once\n", encoding="utf-8")
        return root

    def test_reads_the_spelling_from_the_installed_headers(self):
        for spelling in cs.MIRROR_SPELLINGS:
            with self.subTest(spelling=spelling), tempfile.TemporaryDirectory() as text:
                self.assertEqual(cs.mirror_spelling(self.make_sdk(Path(text), spelling)), spelling)

    def test_refuses_an_sdk_with_no_mirror_or_both_spellings(self):
        for spellings in ((), cs.MIRROR_SPELLINGS):
            with self.subTest(spellings=spellings), tempfile.TemporaryDirectory() as text:
                with self.assertRaises(ValueError):
                    cs.mirror_spelling(self.make_sdk(Path(text), *spellings))


if __name__ == "__main__":
    unittest.main()
