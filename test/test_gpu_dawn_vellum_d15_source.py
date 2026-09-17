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
    require(cmake_text, "find_package(Vellum 0.1.7 EXACT CONFIG QUIET", cmake)
    require(cmake_text, "Vellum::Gpu", cmake)
    require(cmake_text, "Vellum::DawnHeaders", cmake)
    require(cmake_text, "PULP_GPU_AUDIO_HAS_VELLUM_D15=1", cmake)
    require(
        cmake_text,
        "1ab0aafdcfce2e682d8ae65386d93250f2c49d0431561f34a3d7705138d103d3",
        cmake,
    )
    require(cmake_text, "PULP_GPU_AUDIO_VELLUM_D15_RELEASE_ELIGIBLE FALSE", cmake)
    capability_branch = cmake_text.index("if(APPLE AND NOT PULP_IOS")
    for derived_cache_key in (
        "PULP_GPU_AUDIO_HAS_VELLUM_D15 FALSE CACHE INTERNAL",
        'PULP_GPU_AUDIO_VELLUM_RUNTIME_NAME "" CACHE INTERNAL',
        'PULP_GPU_AUDIO_VELLUM_NOTICE_DIR "" CACHE INTERNAL',
        "PULP_GPU_AUDIO_VELLUM_D15_RELEASE_ELIGIBLE FALSE CACHE INTERNAL",
    ):
        if cmake_text.index(derived_cache_key) > capability_branch:
            raise SystemExit(
                "D15 source contract requires derived cache reset before "
                f"platform capability detection: {derived_cache_key}"
            )
    for attribution_digest in (
        "35de8f3cebd71bad85e4d9741f3d03cc8deb32349a6568c77c5f4ef11c2911aa",
        "6c743360892bf2320833833501e78128860dee8a506f61ea7c944b6bc6ff7493",
        "411ad08d92bcfffd2ef93987bc0aeb983bea9a7ff952319d14cd50d48e4b85d2",
    ):
        require(cmake_text, attribution_digest, cmake)

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
