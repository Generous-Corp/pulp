#!/usr/bin/env python3
"""Negative controls for the WAM/WebCLAP timeline source-closure gate."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

import web_timeline_source_closure_check as checker


SOURCES = {
    "timebase": "compiled_time.cpp",
    "timeline": "model.cpp",
    "playback": "transport.cpp",
}


def lane_text(prefix: str, target: str) -> str:
    root = f"_PULP_{prefix}_ROOT"
    sources = f"_PULP_{prefix}_CORE_SOURCES"
    return f"""\
set({sources}
    ${{{root}}}/core/timebase/src/compiled_time.cpp
    ${{{root}}}/core/timeline/src/model.cpp
    ${{{root}}}/core/playback/src/transport.cpp
    ${{{root}}}/core/audio/src/rolling_audio_capture_buffer.cpp
    ${{{root}}}/core/runtime/src/sha256.cpp
)
add_library({target} OBJECT ${{{sources}}})
target_compile_features({target} PUBLIC cxx_std_20)
target_compile_definitions({target} PUBLIC
    PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1)
target_compile_options({target} PRIVATE -fno-exceptions -fno-rtti)
"""


class SourceClosureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        for subsystem, source in SOURCES.items():
            directory = self.root / "core" / subsystem / "src"
            directory.mkdir(parents=True)
            (directory / source).write_text("// fixture\n", encoding="utf-8")
        for source in checker.PORTABLE_DEPENDENCIES:
            path = self.root / source
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("// fixture\n", encoding="utf-8")
        cmake = self.root / "tools" / "cmake"
        cmake.mkdir(parents=True)
        (cmake / "PulpWam.cmake").write_text(
            lane_text("WAM", "pulp-wam-dsp"), encoding="utf-8")
        (cmake / "PulpWclap.cmake").write_text(
            lane_text("WCLAP", "pulp-wclap-dsp"), encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def check(self) -> list[str]:
        return checker.check_root(self.root)[1]

    def mutate(self, relative: str, old: str, new: str) -> None:
        path = self.root / relative
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text)
        path.write_text(text.replace(old, new), encoding="utf-8")

    def test_valid_fixture_passes(self) -> None:
        self.assertEqual(self.check(), [])

    def test_identical_omission_in_both_lists_fails(self) -> None:
        for filename in ("PulpWam.cmake", "PulpWclap.cmake"):
            self.mutate(f"tools/cmake/{filename}",
                        "    ${_PULP_" + ("WAM" if filename == "PulpWam.cmake" else "WCLAP") +
                        "_ROOT}/core/playback/src/transport.cpp\n", "")
        failures = self.check()
        self.assertEqual(sum("missing core/playback/src/transport.cpp" in failure
                             for failure in failures), 2)

    def test_new_production_translation_unit_is_independently_discovered(self) -> None:
        (self.root / "core" / "timeline" / "src" / "future.cpp").write_text(
            "// fixture\n", encoding="utf-8")
        failures = self.check()
        self.assertEqual(sum("missing core/timeline/src/future.cpp" in failure
                             for failure in failures), 2)

    def test_recording_commit_portable_dependencies_are_required(self) -> None:
        for filename in ("PulpWam.cmake", "PulpWclap.cmake"):
            prefix = "WAM" if filename == "PulpWam.cmake" else "WCLAP"
            self.mutate(
                f"tools/cmake/{filename}",
                f"    ${{_PULP_{prefix}_ROOT}}/core/runtime/src/sha256.cpp\n",
                "")
        failures = self.check()
        self.assertEqual(
            sum("missing portable dependency core/runtime/src/sha256.cpp" in failure
                for failure in failures),
            2)

    def test_commented_source_is_missing(self) -> None:
        self.mutate("tools/cmake/PulpWam.cmake",
                    "    ${_PULP_WAM_ROOT}/core/playback/src/transport.cpp",
                    "    # ${_PULP_WAM_ROOT}/core/playback/src/transport.cpp")
        self.assertTrue(any("WAM: missing core/playback/src/transport.cpp" in failure
                            for failure in self.check()))

    def test_block_commented_source_is_missing(self) -> None:
        self.mutate("tools/cmake/PulpWam.cmake",
                    "    ${_PULP_WAM_ROOT}/core/playback/src/transport.cpp",
                    "#[=[\n    ${_PULP_WAM_ROOT}/core/playback/src/transport.cpp\n]=]")
        self.assertTrue(any("WAM: missing core/playback/src/transport.cpp" in failure
                            for failure in self.check()))

    def test_detached_source_list_fails(self) -> None:
        self.mutate("tools/cmake/PulpWam.cmake",
                    "add_library(pulp-wam-dsp OBJECT ${_PULP_WAM_CORE_SOURCES})",
                    "add_library(pulp-wam-dsp OBJECT ${_PULP_WAM_SINGLE_ENTRY})")
        self.assertTrue(any("OBJECT library consuming ${_PULP_WAM_CORE_SOURCES}" in failure
                            for failure in self.check()))

    def test_comment_only_contract_tokens_fail(self) -> None:
        self.mutate("tools/cmake/PulpWclap.cmake",
                    "target_compile_features(pulp-wclap-dsp PUBLIC cxx_std_20)\n"
                    "target_compile_definitions(pulp-wclap-dsp PUBLIC\n"
                    "    PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1)\n"
                    "target_compile_options(pulp-wclap-dsp PRIVATE "
                    "-fno-exceptions -fno-rtti)",
                    "# displaced tokens: cxx_std_20 "
                    "PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1 "
                    "-fno-exceptions -fno-rtti")
        failures = self.check()
        self.assertTrue(any("missing compile feature cxx_std_20" in failure
                            for failure in failures))
        self.assertTrue(any("missing compile definition" in failure for failure in failures))
        self.assertTrue(any("missing compile options" in failure for failure in failures))

    def test_contract_tokens_on_wrong_target_fail(self) -> None:
        self.mutate("tools/cmake/PulpWam.cmake", "pulp-wam-dsp PUBLIC cxx_std_20",
                    "other-target PUBLIC cxx_std_20")
        self.mutate("tools/cmake/PulpWam.cmake",
                    "pulp-wam-dsp PUBLIC\n    PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1",
                    "other-target PUBLIC\n    PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1")
        self.mutate("tools/cmake/PulpWam.cmake",
                    "pulp-wam-dsp PRIVATE -fno-exceptions -fno-rtti",
                    "other-target PRIVATE -fno-exceptions -fno-rtti")
        failures = self.check()
        self.assertTrue(any("missing compile feature cxx_std_20" in failure
                            for failure in failures))
        self.assertTrue(any("missing compile definition" in failure for failure in failures))
        self.assertTrue(any("missing compile options" in failure for failure in failures))


if __name__ == "__main__":
    unittest.main(verbosity=2)
