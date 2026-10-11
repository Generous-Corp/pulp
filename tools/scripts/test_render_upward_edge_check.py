#!/usr/bin/env python3
"""Rule-by-rule tests for render_upward_edge_check.py.

Each case builds a minimal tree that passes, applies one mutation reproducing a
real way the render/app-shell split can regress, and asserts the checker names
it. The passing baseline is asserted too, so a checker that rejects everything
cannot pass this suite.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import render_upward_edge_check as check  # noqa: E402

JNI_CMAKE = """if(ANDROID)
    add_library(pulp-jni SHARED core/platform/src/android/jni_bridge.cpp)
    target_sources(pulp-jni PRIVATE
        core/render/platform/android/gpu_surface_android.cpp
        core/render/platform/android/gpu_surface_android_jni.cpp
    )
endif()
"""

ANDROID_CMAKE = """function(pulp_wire_android_sources)
    # gpu_surface_android.cpp is deliberately not added to pulp-render.
    target_sources(pulp-render PRIVATE ${dir}/choreographer_android.cpp)
endfunction()
"""


class RenderUpwardEdgeRules(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        self.write("core/render/src/skia_surface.cpp",
                   "#include <pulp/render/skia_surface.hpp>\n#include <pulp/canvas/canvas.hpp>\n")
        self.write("core/render/src/renderer3d.cpp", "#include <pulp/scene/scene.hpp>\n")
        self.write("core/render/src/local.cpp", '#include "../include/pulp/render/x.hpp"\n')
        self.write("core/render/include/pulp/render/x.hpp", "#pragma once\n")
        for rel in check.SHELL_SOURCES + check.SHELL_HEADERS:
            self.write(rel, "#include <pulp/view/view.hpp>\n"
                            '#include "../../../../core/audio/platform/android/demo_synth.hpp"\n')
        self.write("CMakeLists.txt", JNI_CMAKE)
        self.write("tools/cmake/PulpAndroid.cmake", ANDROID_CMAKE)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def codes(self) -> list[str]:
        violations, _ = check.check(self.repo)
        return sorted(v.split(":", 1)[0] for v in violations)

    def test_baseline_passes(self) -> None:
        self.assertEqual(self.codes(), [])
        self.assertEqual(check.main(["--repo", str(self.repo)]), 0)

    def test_renderer_including_view_fails(self) -> None:
        self.write("core/render/src/skia_surface.cpp", "#include <pulp/view/view.hpp>\n")
        self.assertEqual(self.codes(), ["render-includes-upward"])
        self.assertEqual(check.main(["--repo", str(self.repo)]), 1)

    def test_renderer_including_state_fails(self) -> None:
        self.write("core/render/platform/android/choreographer_android.cpp",
                   "#include <pulp/state/store.hpp>\n")
        self.assertEqual(self.codes(), ["render-includes-upward"])

    def test_relative_include_escaping_render_fails(self) -> None:
        self.write("core/render/src/local.cpp",
                   '#include "../../audio/platform/android/demo_synth.hpp"\n')
        self.assertEqual(self.codes(), ["render-includes-outside"])

    def test_shell_added_back_to_render_fails(self) -> None:
        self.write("tools/cmake/PulpAndroid.cmake", ANDROID_CMAKE.replace(
            "${dir}/choreographer_android.cpp",
            "${dir}/choreographer_android.cpp\n        ${dir}/gpu_surface_android.cpp"))
        self.assertEqual(self.codes(), ["shell-in-render"])

    def test_shell_named_only_in_a_comment_passes(self) -> None:
        self.assertNotIn("shell-in-render", self.codes())

    def test_shell_dropped_from_jni_fails(self) -> None:
        self.write("CMakeLists.txt", JNI_CMAKE.replace(
            "        core/render/platform/android/gpu_surface_android_jni.cpp\n", ""))
        self.assertEqual(self.codes(), ["shell-not-in-jni"])

    def test_missing_shell_file_is_inconclusive(self) -> None:
        (self.repo / check.SHELL_SOURCES[0]).unlink()
        self.assertEqual(check.main(["--repo", str(self.repo)]), 2)

    def test_missing_render_dir_is_inconclusive(self) -> None:
        self.assertEqual(check.main(["--repo", str(self.repo / "nowhere")]), 2)


if __name__ == "__main__":
    unittest.main()
