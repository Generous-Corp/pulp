#!/usr/bin/env python3
"""Self-test for pch_wiring_check.py against synthetic generator output.

Each case builds a small fake build directory (CMakeCache.txt + build.ninja or
flags.make + the ledger) and asserts the check accepts a correctly wired tree
and rejects each specific way the wiring can be wrong: a ledger claim the
flags do not back, a PCH on an excluded target or on SDL3-static, a -std
mismatch, a carrier definition the consumer lacks, and a PCH present while
the option is OFF.
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pch_wiring_check as pwc  # noqa: E402

PCH20 = "CMakeFiles/pulp-test-pch-cxx20.dir/cmake_pch.hxx"
PCH23 = "CMakeFiles/pulp-test-pch-cxx23.dir/cmake_pch.hxx"


def _use(pch: str) -> str:
    return f"-Xclang -include-pch -Xclang /b/{pch}.pch -Xclang -include -Xclang /b/{pch}"


def ninja_tree(
    *,
    biquad_flags: str = f"-O3 -std=gnu++20 {_use(PCH20)}",
    biquad_defines: str = "-DFIXTURE=\\\"/w/test\\\"",
    headless_flags: str = f"-O3 -std=gnu++23 {_use(PCH23)}",
    noexc_flags: str = "-O3 -std=gnu++20 -fno-exceptions",
    sdl_flags: str = "-O3 -std=gnu11",
    carrier20_defines: str = "",
    carrier23_defines: str = "",
) -> str:
    return "\n".join([
        "rule CXX_COMPILER__x",
        "  command = c++ $FLAGS $DEFINES -c $in -o $out",
        f"build {PCH20}.pch: CXX_COMPILER__pulp-test-pch-cxx20_Release /b/pulp-test-pch/cxx20_stub.cpp",
        "  FLAGS = -O3 -std=gnu++20 -Xclang -fno-pch-timestamp -Xclang -emit-pch -x c++-header",
        f"  DEFINES = {carrier20_defines}",
        f"build {PCH23}.pch: CXX_COMPILER__pulp-test-pch-cxx23_Release /b/pulp-test-pch/cxx23_stub.cpp",
        "  FLAGS = -O3 -std=gnu++23 -Xclang -fno-pch-timestamp -Xclang -emit-pch -x c++-header",
        f"  DEFINES = {carrier23_defines}",
        "build test/CMakeFiles/pulp-test-biquad.dir/test_biquad.cpp.o: CXX_COMPILER__pulp-test-biquad_Release /w/test/test_biquad.cpp",
        f"  FLAGS = {biquad_flags}",
        f"  DEFINES = {biquad_defines}",
        "build test/CMakeFiles/pulp-test-headless.dir/test_headless.cpp.o: CXX_COMPILER__pulp-test-headless_Release /w/test/test_headless.cpp",
        f"  FLAGS = {headless_flags}",
        "  DEFINES = ",
        "build test/CMakeFiles/pulp-test-signal-no-exceptions.dir/test_signal_no_exceptions.cpp.o: CXX_COMPILER__pulp-test-signal-no-exceptions_Release /w/test/x.cpp",
        f"  FLAGS = {noexc_flags}",
        "  DEFINES = ",
        "build _deps/sdl3-build/CMakeFiles/SDL3-static.dir/src/SDL.c.o: C_COMPILER__SDL3-static_Release /d/src/SDL.c",
        f"  FLAGS = {sdl_flags}",
        "  DEFINES = -DSDL_STATIC_LIB",
        "build test/pulp-test-biquad: CXX_EXECUTABLE_LINKER__pulp-test-biquad_Release test/CMakeFiles/pulp-test-biquad.dir/test_biquad.cpp.o",
        "  FLAGS = -O3",
        "",
    ])


LEDGER_ON = "\n".join([
    "# target\tstatus\tdetail",
    "pulp-test-biquad\tpch\tpulp-test-pch-cxx20",
    "pulp-test-headless\tpch\tpulp-test-pch-cxx23",
    "pulp-test-signal-no-exceptions\tskip\tcompile-option:-fno-exceptions",
    "",
])
LEDGER_OFF = "\n".join([
    "pulp-test-biquad\toff\tPULP_TEST_PCH=OFF",
    "pulp-test-headless\toff\tPULP_TEST_PCH=OFF",
    "pulp-test-signal-no-exceptions\toff\tPULP_TEST_PCH=OFF",
    "",
])
EXPECT = {
    "pulp-test-biquad": "pulp-test-pch-cxx20",
    "pulp-test-headless": "pulp-test-pch-cxx23",
    "pulp-test-signal-no-exceptions": "none",
    "SDL3-static": "none",
}


class PchWiringCheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.build = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write(self, generator: str, body: str, ledger: str) -> None:
        (self.build / "CMakeCache.txt").write_text(f"CMAKE_GENERATOR:INTERNAL={generator}\n")
        (self.build / pwc.LEDGER_NAME).write_text(ledger)
        if generator == "Ninja":
            (self.build / "build.ninja").write_text(body)

    def test_correct_ninja_tree_passes(self) -> None:
        self._write("Ninja", ninja_tree(), LEDGER_ON)
        self.assertEqual(pwc.check(self.build, True, EXPECT), [])

    def test_option_off_tree_passes(self) -> None:
        plain = ninja_tree(biquad_flags="-O3 -std=gnu++20", headless_flags="-O3 -std=gnu++23")
        self._write("Ninja", plain, LEDGER_OFF)
        self.assertEqual(pwc.check(self.build, False, EXPECT), [])

    def test_ledger_claim_without_flags_fails(self) -> None:
        self._write("Ninja", ninja_tree(biquad_flags="-O3 -std=gnu++20"), LEDGER_ON)
        problems = pwc.check(self.build, True, EXPECT)
        self.assertTrue(any("pulp-test-biquad" in p and "no PCH" in p for p in problems), problems)

    def test_pch_on_excluded_target_fails(self) -> None:
        self._write("Ninja", ninja_tree(noexc_flags=f"-O3 -std=gnu++20 -fno-exceptions {_use(PCH20)}"), LEDGER_ON)
        problems = pwc.check(self.build, True, EXPECT)
        self.assertTrue(any("pulp-test-signal-no-exceptions" in p for p in problems), problems)

    def test_sdl3_with_pch_fails(self) -> None:
        sdl = "-O3 -std=gnu11 -Xclang -include-pch -Xclang /b/_deps/sdl3-build/CMakeFiles/SDL3-static.dir/cmake_pch.h.pch"
        self._write("Ninja", ninja_tree(sdl_flags=sdl), LEDGER_ON)
        problems = pwc.check(self.build, True, EXPECT)
        self.assertTrue(any("SDL3-static=none" in p for p in problems), problems)

    def test_wrong_carrier_fails(self) -> None:
        self._write("Ninja", ninja_tree(headless_flags=f"-O3 -std=gnu++23 {_use(PCH20)}"), LEDGER_ON)
        problems = pwc.check(self.build, True, EXPECT)
        self.assertTrue(any("pulp-test-headless" in p and "pulp-test-pch-cxx20" in p for p in problems), problems)

    def test_std_mismatch_fails(self) -> None:
        self._write("Ninja", ninja_tree(biquad_flags=f"-O3 -std=c++20 {_use(PCH20)}"), LEDGER_ON)
        problems = pwc.check(self.build, True, EXPECT)
        self.assertTrue(any("-std" in p and "pulp-test-biquad" in p for p in problems), problems)

    def test_carrier_define_missing_on_consumer_fails(self) -> None:
        self._write("Ninja", ninja_tree(carrier20_defines="-DCARRIER_ONLY=1"), LEDGER_ON)
        problems = pwc.check(self.build, True, EXPECT)
        self.assertTrue(any("CARRIER_ONLY" in p for p in problems), problems)

    def test_consumer_extra_define_is_fine(self) -> None:
        self._write("Ninja", ninja_tree(biquad_defines="-DFIXTURE=1 -DEXTRA=2"), LEDGER_ON)
        self.assertEqual(pwc.check(self.build, True, EXPECT), [])

    def test_option_on_with_no_pch_targets_fails(self) -> None:
        self._write("Ninja", ninja_tree(), LEDGER_OFF)
        problems = pwc.check(self.build, True, {})
        self.assertTrue(any("records no target" in p for p in problems), problems)

    def test_option_off_with_pch_fails(self) -> None:
        self._write("Ninja", ninja_tree(), LEDGER_ON)
        problems = pwc.check(self.build, False, {})
        self.assertTrue(any("PULP_TEST_PCH=OFF" in p for p in problems), problems)

    def test_makefiles_tree_passes(self) -> None:
        self._write("Unix Makefiles", "", LEDGER_ON)

        def flags(target: str, sub: str, lang: str, f: str, d: str = "") -> None:
            d_ = self.build / sub / "CMakeFiles" / f"{target}.dir"
            d_.mkdir(parents=True, exist_ok=True)
            (d_ / "flags.make").write_text(f"{lang}_FLAGS = {f}\n{lang}_DEFINES = {d}\n")

        flags("pulp-test-pch-cxx20", ".", "CXX", "-O3 -std=gnu++20 -Xclang -emit-pch")
        flags("pulp-test-pch-cxx23", ".", "CXX", "-O3 -std=gnu++23 -Xclang -emit-pch")
        flags("pulp-test-biquad", "test", "CXX", f"-O3 -std=gnu++20 {_use(PCH20)}", "-DFIXTURE=1")
        flags("pulp-test-headless", "test", "CXX", f"-O3 -std=gnu++23 {_use(PCH23)}")
        flags("pulp-test-signal-no-exceptions", "test", "CXX", "-O3 -std=gnu++20 -fno-exceptions")
        flags("SDL3-static", "_deps/sdl3-build", "C", "-O3 -std=gnu11")
        self.assertEqual(pwc.check(self.build, True, EXPECT), [])
        # And the same tree with SDL3's PCH back on is rejected.
        flags("SDL3-static", "_deps/sdl3-build", "C", "-O3 -Xclang -include-pch -Xclang x/cmake_pch.h.pch")
        problems = pwc.check(self.build, True, EXPECT)
        self.assertTrue(any("SDL3-static=none" in p for p in problems), problems)

    def test_missing_ledger_is_an_error(self) -> None:
        (self.build / "CMakeCache.txt").write_text("CMAKE_GENERATOR:INTERNAL=Ninja\n")
        (self.build / "build.ninja").write_text(ninja_tree())
        with self.assertRaises(SystemExit):
            pwc.check(self.build, True, {})


if __name__ == "__main__":
    unittest.main()
