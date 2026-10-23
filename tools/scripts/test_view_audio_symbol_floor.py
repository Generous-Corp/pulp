#!/usr/bin/env python3
"""Positive and negative controls for view_audio_symbol_floor.py.

A fake `nm` serves canned `nm -A -P -g` listings in both the macOS and the GNU
spelling, so the gate's verdicts are checked without building archives.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import stat
import sys
import tempfile
import unittest

MODULE_PATH = pathlib.Path(__file__).with_name("view_audio_symbol_floor.py")
SPEC = importlib.util.spec_from_file_location("view_audio_symbol_floor", MODULE_PATH)
floor = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = floor
SPEC.loader.exec_module(floor)
sys.path.insert(0, str(MODULE_PATH.parent))
import nm_symbols  # noqa: E402

AUDIO = (
    "libpulp-view-audio.a[visualizers.cpp.o]: __ZTIN4pulp4view5MeterE S 10 0\n"
    "libpulp-view-audio.a[visualizers.cpp.o]: __ZN4pulp4view5Meter9set_levelEff T 20 0\n"
    "libpulp-view-audio.a[visualizers.cpp.o]: __ZN4pulp4view4View5paintEv U 0 0\n"
)
# GNU nm spelling: no leading underscore, same field layout.
CORE_DIRTY = (
    "libpulp-view-core.a[inspector.cpp.o]: _ZN4pulp4view4View5paintEv T 0 0\n"
    "libpulp-view-core.a[inspector.cpp.o]: __ZTIN4pulp4view5MeterE U 0 0\n"
)
CORE_CLEAN = "libpulp-view-core.a[inspector.cpp.o]: __ZN4pulp4view4View5paintEv T 0 0\n"
CORE_WEAK = "libpulp-view-core.a[inspector.cpp.o]: __ZTIN4pulp4view5MeterE w 0 0\n"


class SymbolFloorTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        self.nm = self.dir / "fake-nm"
        self.nm.write_text(
            "#!/bin/sh\n"
            'for last; do :; done\n'
            'cat "$last.listing" 2>/dev/null || { echo "no such archive" >&2; exit 1; }\n',
            encoding="utf-8")
        self.nm.chmod(self.nm.stat().st_mode | stat.S_IEXEC)
        self._archive("audio.a", AUDIO)

    def tearDown(self):
        self.tmp.cleanup()

    def _archive(self, name: str, listing: str) -> str:
        path = self.dir / name
        path.write_text("", encoding="utf-8")
        (self.dir / f"{name}.listing").write_text(listing, encoding="utf-8")
        return str(path)

    def _run(self, *extra: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            rc = floor.main(["--nm", str(self.nm), "--audio", str(self.dir / "audio.a"), *extra])
        return rc, out.getvalue()

    def test_a_reference_to_view_audio_code_fails(self):
        core = self._archive("core.a", CORE_DIRTY)
        rc, out = self._run("--check", f"view-core={core}")
        self.assertEqual(rc, 1, out)
        self.assertIn("__ZTIN4pulp4view5MeterE", out)

    def test_a_weak_undefined_reference_fails(self):
        core = self._archive("core.a", CORE_WEAK)
        rc, out = self._run("--check", f"view-core={core}")
        self.assertEqual(rc, 1, out)

    def test_a_clean_library_passes(self):
        core = self._archive("core.a", CORE_CLEAN)
        rc, out = self._run("--check", f"view-core={core}")
        self.assertEqual(rc, 0, out)

    def test_an_undefined_symbol_view_audio_also_needs_is_not_a_violation(self):
        # View::paint is undefined in the audio archive too; only symbols the
        # audio archive DEFINES count.
        core = self._archive("core.a", "libpulp-view-core.a[x.o]: _ZN4pulp4view4View5paintEv U 0 0\n")
        rc, out = self._run("--check", f"view-core={core}")
        self.assertEqual(rc, 0, out)

    def test_a_matching_debt_passes_and_a_stale_debt_fails(self):
        dirty = self._archive("script.a", CORE_DIRTY)
        rc, out = self._run("--check", f"view-script={dirty}", "--debt", "view-script=P19")
        self.assertEqual(rc, 0, out)
        clean = self._archive("clean.a", CORE_CLEAN)
        rc, out = self._run("--check", f"view-script={clean}", "--debt", "view-script=P19")
        self.assertEqual(rc, 1, out)
        self.assertIn("no longer matches", out)

    def test_an_audio_archive_that_defines_nothing_is_a_scan_error(self):
        self._archive("audio.a", "")
        core = self._archive("core.a", CORE_DIRTY)
        rc, _ = self._run("--check", f"view-core={core}")
        self.assertEqual(rc, 2)

    def test_weak_definitions_count_toward_the_floor(self):
        # One strong and two weak definitions: the floor is all three.
        listing = (
            "libpulp-view-audio.a[meter.cpp.o]: __ZN4pulp4view5Meter9set_levelEff T 20 0\n"
            "libpulp-view-audio.a[meter.cpp.o]: __ZTIN4pulp4view5MeterE W 0 0\n"
            "libpulp-view-audio.a[meter.cpp.o]: __ZTVN4pulp4view5MeterE W 0 0\n"
            "libpulp-view-audio.a[meter.cpp.o]: __ZN4pulp4view4View5paintEv U 0 0\n"
        )
        self.assertEqual(len(nm_symbols.defined_any(listing)), 3)
        self.assertEqual(len(nm_symbols.defined_strong(listing)), 1)
        self._archive("audio.a", listing)
        for symbol in ("__ZTIN4pulp4view5MeterE", "__ZTVN4pulp4view5MeterE"):
            core = self._archive("core.a", f"libpulp-view-core.a[x.o]: {symbol} U 0 0\n")
            rc, out = self._run("--check", f"view-core={core}")
            self.assertEqual(rc, 1, out)
            self.assertIn(symbol, out)

    def test_the_floor_is_built_from_weak_definitions_alone(self):
        # An archive whose only definitions are weak is not "defines nothing".
        self._archive("audio.a", "libpulp-view-audio.a[m.o]: __ZTIN4pulp4view5MeterE W 0 0\n")
        core = self._archive("core.a", CORE_DIRTY)
        rc, out = self._run("--check", f"view-core={core}")
        self.assertEqual(rc, 1, out)

    def test_the_registered_gate_counts_weak_definitions(self):
        # The ctest calls archive_symbol_closure_check.py directly, so it must
        # ask for the same rule this wrapper uses.
        manifest = MODULE_PATH.parents[2] / "test/cmake/view_host_tests.cmake"
        text = manifest.read_text(encoding="utf-8")
        block = text.split("add_test(NAME pulp-view-audio-symbol-floor\n", 1)[1].split(")\n", 1)[0]
        self.assertIn('--forbidden "$<TARGET_FILE:pulp-view-audio>" --forbidden-defines any', block)

    def test_a_missing_archive_is_a_scan_error(self):
        rc, _ = self._run("--check", f"view-core={self.dir / 'missing.a'}")
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
