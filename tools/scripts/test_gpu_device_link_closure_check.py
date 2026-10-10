#!/usr/bin/env python3
"""Rule-by-rule tests for gpu_device_link_closure_check.py.

Each case starts from a properties set that passes, applies one mutation that
reproduces a real way the GPU device layering can break, and asserts that the
checker reports that violation's code and nothing else. The baseline passing is
asserted too, so a checker that rejects everything cannot pass this suite.
"""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gpu_device_link_closure_check as check  # noqa: E402


def baseline_properties(root: Path) -> dict[str, str]:
    return {
        "api.type": "INTERFACE_LIBRARY",
        "api.interface_link": "",
        "device.type": "STATIC_LIBRARY",
        "device.link": "pulp::gpu-device-api;pulp::runtime;-framework Metal;skia::skia;skia::dawn",
        "device.sources": "src/gpu_compute.cpp;src/gpu_surface_dawn.cpp",
        "device.source_dir": str(root / "render"),
        "render.link": "pulp::runtime;pulp::canvas;pulp::gpu-device-api;pulp::gpu-device;skia::skia",
        "render.interface_link":
            "pulp::runtime;pulp::canvas;pulp::gpu-device-api;"
            "$<LINK_ONLY:pulp::gpu-device>;skia::skia",
        "gpu_audio.link": "pulp::audio;pulp::runtime;$<LINK_ONLY:Threads::Threads>;"
                          "pulp::gpu-device;pulp::signal",
        "gpu_audio.source_dir": str(root / "gpu_audio"),
        "gpu_audio.expects_device": "1",
        "api_consumer.link": "pulp::gpu-device-api",
        "device_consumer.link": "pulp::gpu-device",
    }


def write_tree(root: Path) -> Path:
    include = root / "render" / "include"
    for header in check.API_HEADERS:
        path = include / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#pragma once\n#include <cstdint>\n", encoding="utf-8")
    (include / "pulp/render/gpu_compute.hpp").write_text(
        "#pragma once\n#include <pulp/render/gpu_surface.hpp>\n", encoding="utf-8")
    src = root / "render" / "src"
    src.mkdir(parents=True)
    (src / "gpu_compute.cpp").write_text(
        '#include <pulp/render/gpu_compute.hpp>\n#include <pulp/runtime/log.hpp>\n'
        '#include "dawn/native/DawnNative.h"\n', encoding="utf-8")
    (src / "gpu_surface_dawn.cpp").write_text(
        "#include <pulp/render/gpu_surface.hpp>\n", encoding="utf-8")
    audio = root / "gpu_audio" / "src"
    audio.mkdir(parents=True)
    (audio / "gpu_convolver.cpp").write_text(
        "#include <pulp/render/gpu_compute.hpp>\n#include <pulp/signal/fft.hpp>\n",
        encoding="utf-8")
    return include


class LinkClosureRules(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.include = write_tree(self.root)
        self.props = baseline_properties(self.root)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def codes(self) -> list[str]:
        return sorted(v.code for v in
                      check.check_links(self.props) + check.check_includes(self.props, self.include))

    def run_main(self) -> tuple[int, str]:
        path = self.root / "props.txt"
        path.write_text("".join(f"{k}\t{v}\n" for k, v in self.props.items()), encoding="utf-8")
        stderr = io.StringIO()
        with redirect_stdout(io.StringIO()), redirect_stderr(stderr):
            code = check.main(["--properties", str(path), "--render-include", str(self.include)])
        return code, stderr.getvalue()

    def test_baseline_passes(self) -> None:
        self.assertEqual(self.codes(), [])
        self.assertEqual(self.run_main()[0], 0)

    def test_gpu_audio_linking_renderer_fails(self) -> None:
        self.props["gpu_audio.link"] = self.props["gpu_audio.link"].replace(
            "pulp::gpu-device", "pulp::render")
        self.assertEqual(self.codes(), ["gpu-audio-missing-device", "gpu-audio-reaches-render"])
        code, stderr = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("gpu-audio-reaches-render", stderr)

    def test_api_gaining_a_link_fails(self) -> None:
        self.props["api.interface_link"] = "skia::dawn"
        self.assertEqual(self.codes(), ["api-links-something"])

    def test_api_as_static_library_fails(self) -> None:
        self.props["api.type"] = "STATIC_LIBRARY"
        self.assertEqual(self.codes(), ["api-not-interface"])

    def test_device_linking_canvas_fails(self) -> None:
        self.props["device.link"] += ";pulp::canvas"
        self.assertEqual(self.codes(), ["device-reaches-up"])

    def test_device_without_api_fails(self) -> None:
        self.props["device.link"] = "pulp::runtime"
        self.assertEqual(self.codes(), ["device-missing-api"])

    def test_render_public_device_fails(self) -> None:
        self.props["render.interface_link"] = self.props["render.interface_link"].replace(
            "$<LINK_ONLY:pulp::gpu-device>", "pulp::gpu-device")
        self.assertEqual(self.codes(), ["render-device-public"])

    def test_render_dropping_device_fails(self) -> None:
        self.props["render.link"] = "pulp::runtime;pulp::canvas;pulp::gpu-device-api"
        self.props["render.interface_link"] = "pulp::runtime;pulp::canvas;pulp::gpu-device-api"
        self.assertEqual(self.codes(), ["render-device-unlinked", "render-missing-device"])

    def test_consumer_with_extra_module_fails(self) -> None:
        self.props["device_consumer.link"] = "pulp::gpu-device;pulp::render"
        self.assertEqual(self.codes(), ["consumer-not-isolated"])

    def test_api_header_including_renderer_fails(self) -> None:
        (self.include / "pulp/render/gpu_surface.hpp").write_text(
            "#pragma once\n#include <pulp/render/skia_surface.hpp>\n", encoding="utf-8")
        self.assertEqual(self.codes(), ["api-header-reaches-out"])

    def test_api_header_naming_dawn_fails(self) -> None:
        (self.include / "pulp/render/gpu_compute.hpp").write_text(
            '#pragma once\n#include "webgpu/webgpu_cpp.h"\n', encoding="utf-8")
        self.assertEqual(self.codes(), ["api-header-names-provider"])

    def test_device_source_including_canvas_fails(self) -> None:
        (self.root / "render/src/gpu_surface_dawn.cpp").write_text(
            "#include <pulp/canvas/canvas.hpp>\n", encoding="utf-8")
        self.assertEqual(self.codes(), ["device-source-reaches-up"])

    def test_gpu_audio_including_renderer_header_fails(self) -> None:
        (self.root / "gpu_audio/src/gpu_convolver.cpp").write_text(
            "#include <pulp/render/skia_surface.hpp>\n", encoding="utf-8")
        self.assertEqual(self.codes(), ["gpu-audio-includes-renderer"])

    def test_missing_api_header_fails(self) -> None:
        (self.include / "pulp/render/gpu_render_time.hpp").unlink()
        self.assertEqual(self.codes(), ["api-header-missing"])

    def test_incomplete_properties_is_inconclusive(self) -> None:
        del self.props["render.link"]
        code, stderr = self.run_main()
        self.assertEqual(code, 2)
        self.assertIn("INCONCLUSIVE", stderr)

    def test_split_list_keeps_generator_expressions_whole(self) -> None:
        self.assertEqual(
            check.split_list("a;$<LINK_ONLY:$<BUILD_INTERFACE:b;c>>;d"),
            ["a", "$<LINK_ONLY:$<BUILD_INTERFACE:b;c>>", "d"])


if __name__ == "__main__":
    unittest.main()
