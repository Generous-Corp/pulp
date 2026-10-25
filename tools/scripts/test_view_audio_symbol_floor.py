#!/usr/bin/env python3
"""Positive and negative controls for view_audio_symbol_floor.py.

A fake `nm` serves canned `nm -A -P -g` listings in both the macOS and the GNU
spelling, so the gate's verdicts are checked without building archives.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest

MODULE_PATH = pathlib.Path(__file__).with_name("view_audio_symbol_floor.py")
SPEC = importlib.util.spec_from_file_location("view_audio_symbol_floor", MODULE_PATH)
floor = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = floor
SPEC.loader.exec_module(floor)
FIXTURES = MODULE_PATH.with_name("fixtures") / "nm_symbols"
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
            # Like Apple's nm: `-m` prints darwin format when a capture exists;
            # without one it fails, like a GNU nm that has no `-m`.
            'case " $* " in *" -m "*) exec cat "$last.m.listing" 2>/dev/null || exit 1;; esac\n'
            'cat "$last.listing" 2>/dev/null || { echo "no such archive" >&2; exit 1; }\n',
            encoding="utf-8")
        self.nm.chmod(self.nm.stat().st_mode | stat.S_IEXEC)
        self._archive("audio.a", AUDIO)

    def tearDown(self):
        self.tmp.cleanup()

    def _archive(self, name: str, listing: str, darwin: str | None = None) -> str:
        path = self.dir / name
        path.write_text("", encoding="utf-8")
        (self.dir / f"{name}.listing").write_text(listing, encoding="utf-8")
        darwin_path = self.dir / f"{name}.m.listing"
        if darwin is not None:
            darwin_path.write_text(darwin, encoding="utf-8")
        elif darwin_path.exists():
            darwin_path.unlink()
        return str(path)

    def _fixture(self, name: str, stem: str) -> str:
        """An archive served from real captured `nm -A -P` (and `nm -m`) output."""
        listing = (FIXTURES / f"{stem}.P.txt").read_text(encoding="utf-8")
        darwin = FIXTURES / f"{stem}.m.txt"
        return self._archive(name, listing, darwin.read_text(encoding="utf-8") if darwin.exists() else None)

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

    def test_only_symbol_lines_are_read(self):
        # The spelling is decided once per listing (test_nm_symbols covers
        # that); prose, warnings and headers are skipped in either.
        prefixed = (
            "libpulp-view-audio.a[m.o]:\n"
            "libpulp-view-audio.a[m.o]: __ZTIN4pulp4view5MeterE W 0 0\n"
            "libpulp-view-audio.a[m.o]: undefined_only U\n"
            "warning: something odd happened\n"
            "libpulp-view-audio.a[m.o]: also not a symbol line\n"
        )
        plain = "plain_symbol T 1f 8\nundefined_only U\nnot a symbol line\n"
        self.assertEqual(nm_symbols.defined_any(prefixed), {"__ZTIN4pulp4view5MeterE"})
        self.assertEqual(nm_symbols.defined_any(plain), {"plain_symbol"})
        self.assertEqual(nm_symbols.undefined(prefixed), {"undefined_only"})
        self.assertEqual(nm_symbols.undefined(plain), {"undefined_only"})

    def test_gnu_unique_and_indirect_definitions_count_toward_the_floor(self):
        # GNU nm on ELF: `u` is a unique global and `i` an indirect function,
        # both definitions; Vellum's inertness floor reads the same set.
        listing = (
            "libpulp-view-audio.a[m.o]: _ZN4pulp4view5Meter9set_levelEff T 0000000000000040 0000000000000010\n"
            "libpulp-view-audio.a[m.o]: _ZZN4pulp4view5Meter8instanceEvE1s u 0000000000000000 0000000000000008\n"
            "libpulp-view-audio.a[m.o]: _ZN4pulp4view13resolve_simdEv i 0000000000000080 0000000000000004\n"
            "libpulp-view-audio.a[m.o]: _ZN4pulp4view4View5paintEv U\n"
        )
        self.assertEqual(nm_symbols.defined_any(listing), {
            "_ZN4pulp4view5Meter9set_levelEff", "_ZZN4pulp4view5Meter8instanceEvE1s",
            "_ZN4pulp4view13resolve_simdEv"})
        self.assertEqual(nm_symbols.defined_strong(listing), {"_ZN4pulp4view5Meter9set_levelEff"})
        self._archive("audio.a", listing)
        core = self._archive("core.a", "libpulp-view-core.a[x.o]: _ZZN4pulp4view5Meter8instanceEvE1s U\n")
        rc, out = self._run("--check", f"view-core={core}")
        self.assertEqual(rc, 1, out)

    def test_planted_weak_meter_rtti_is_caught_on_mach_o(self):
        # Real `nm -m` output from compiling planted.cpp (a view-core TU that
        # sees Meter's definition) on macOS: Apple's -P letters say S/D for the
        # weak copies, so only the darwin attributes reveal them.
        self._fixture("audio.a", "darwin_compiled_view_audio")
        core = self._fixture("core.a", "darwin_compiled_view_core_planted")
        rc, out = self._run("--check", f"view-core={core}")
        self.assertEqual(rc, 1, out)
        for text in ("typeinfo for pulp::view::Meter", "typeinfo name for pulp::view::Meter",
                     "vtable for pulp::view::Meter"):
            self.assertIn(f"weak copy: {text}", out)
        for shared in ("std::", "ChannelStripView", "lambda"):
            self.assertNotIn(shared, out)

    def test_planted_weak_meter_rtti_is_caught_on_elf(self):
        self._fixture("audio.a", "elf_view_audio")
        core = self._fixture("core.a", "elf_view_core_planted")
        rc, out = self._run("--check", f"view-core={core}")
        self.assertEqual(rc, 1, out)
        self.assertIn("weak copy: vtable for pulp::view::Meter", out)
        for shared in ("std::", "ChannelStripView", "lambda"):
            self.assertNotIn(shared, out)

    def test_shared_std_function_and_view_core_rtti_stay_clean(self):
        # std::function's __func over a lambda, std::format_error and
        # view-core's own ChannelStripView, all weak, in each format.
        for audio, core_stem in (("darwin_compiled_view_audio", "darwin_compiled_view_core"),
                                 ("darwin_view_audio", "darwin_view_core"),
                                 ("elf_view_audio", "elf_view_core"),
                                 ("elf_ci_shared_rtti_view_audio", "elf_ci_shared_rtti_view_core")):
            with self.subTest(core=core_stem):
                self._fixture("audio.a", audio)
                core = self._fixture("core.a", core_stem)
                rc, out = self._run("--check", f"view-core={core}")
                self.assertEqual(rc, 0, out)

    def test_apple_letters_alone_cannot_see_the_weak_copy(self):
        # The control the darwin read exists for: without `nm -m`, the same
        # Mach-O archive's letters show no weak definition at all.
        listing = (FIXTURES / "darwin_compiled_view_core_planted.P.txt").read_text(encoding="utf-8")
        self.assertEqual(nm_symbols.weak_defined(listing), set())
        self._fixture("audio.a", "darwin_compiled_view_audio")
        core = self._archive("core.a", listing)
        rc, out = self._run("--check", f"view-core={core}")
        self.assertEqual(rc, 0, out)

    def test_the_registered_gate_counts_weak_definitions(self):
        # The ctest calls archive_symbol_closure_check.py directly, so it must
        # ask for the same rules this wrapper uses. Under ctest the registered
        # command is read back from `ctest --show-only=json-v1`; standalone,
        # from the CMake source that registers it.
        build = os.environ.get("PULP_CTEST_BUILD_DIR")
        if build:
            proc = subprocess.run(["ctest", "--test-dir", build, "--show-only=json-v1"],
                                  capture_output=True, text=True, encoding="utf-8", check=True)
            tests = {test["name"]: test for test in json.loads(proc.stdout)["tests"]}
            command = tests["pulp-view-audio-symbol-floor"]["command"]
            for flag in ("--forbidden-defines", "--weak-rtti-contamination", "--no-control"):
                self.assertIn(flag, command)
            self.assertEqual(command[command.index("--forbidden-defines") + 1], "any")
            return
        manifest = MODULE_PATH.parents[2] / "test/cmake/view_host_tests.cmake"
        text = manifest.read_text(encoding="utf-8")
        block = text.split("add_test(NAME pulp-view-audio-symbol-floor\n", 1)[1].split(")\n", 1)[0]
        self.assertIn('--forbidden "$<TARGET_FILE:pulp-view-audio>" --forbidden-defines any', block)
        self.assertIn("--weak-rtti-contamination", block)

    def test_a_missing_archive_is_a_scan_error(self):
        rc, _ = self._run("--check", f"view-core={self.dir / 'missing.a'}")
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
