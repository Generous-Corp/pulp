#!/usr/bin/env python3
"""Keep Pulp's D15 lane an installed Vellum-provider consumer."""

from __future__ import annotations

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]


def require(text: str, needle: str, path: pathlib.Path) -> None:
    if needle not in text:
        raise SystemExit(f"D15 source contract missing {needle!r} in {path.relative_to(ROOT)}")


def main() -> int:
    cmake = ROOT / "core/gpu_audio/CMakeLists.txt"
    provider = ROOT / "core/gpu_audio/src/detail/dawn_shared_io_provider.cpp"
    tests = ROOT / "test/cmake/render_gpu_surface_tests.cmake"

    cmake_text = cmake.read_text(encoding="utf-8")
    require(cmake_text, "PULP_GPU_AUDIO_VELLUM_D15_PREFIX", cmake)
    require(cmake_text, "find_package(Vellum 0.1 CONFIG QUIET", cmake)
    require(cmake_text, "Vellum::Gpu", cmake)
    require(cmake_text, "Vellum::DawnHeaders", cmake)
    require(cmake_text, "PULP_GPU_AUDIO_HAS_VELLUM_D15=1", cmake)

    provider_text = provider.read_text(encoding="utf-8")
    require(provider_text, "<vellum/graphics/dawn_bootstrap.hpp>", provider)
    require(provider_text, "register_dawn_bootstrap", provider)
    require(provider_text, "register_native_dawn_bootstrap", provider)
    require(provider_text, "PULP_GPU_AUDIO_HAS_VELLUM_D15", provider)

    tests_text = tests.read_text(encoding="utf-8")
    require(tests_text, "pulp-test-gpu-dawn-vellum-bootstrap-contract", tests)
    require(tests_text, "pulp-gpu-dawn-vellum-provider-topology", tests)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
