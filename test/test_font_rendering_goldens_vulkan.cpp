// test_font_rendering_goldens_vulkan.cpp — Font v2 Slice 3.4
// (Cross-backend rendering goldens — Skia GPU Vulkan lane, SCAFFOLD).
//
// Sibling of `test_font_rendering_goldens.cpp` (Skia raster) and
// `test_font_rendering_goldens_gpu.cpp` (Skia GPU on Dawn → Metal,
// macOS-arm64 only). This file is the **structural scaffold** for the
// Skia GPU Vulkan lane. It does NOT wire up Vulkan at runtime yet — the
// CMake side gates the target out by default (PULP_VULKAN_AVAILABLE=OFF)
// and the compile-time guards below ensure the TU is a no-op on every
// CI lane Pulp currently runs.
//
// ────────────────────────────────────────────────────────────────
// What this lane would prove (once a Vulkan-capable runner exists):
//
//   * The Skia GPU backend on Vulkan (Graphite-Vk / GrVkBackend
//     depending on Skia point release) produces text rasters in the
//     same neighbourhood as the raster and Metal lanes for the three
//     Slice 3.4 reference strings (`"Hello"` / Inter / 14 px,
//     `"日本語"` / Inter / 14 px CJK fallback, `"Hello world"` /
//     JetBrains Mono / 12 px).
//   * The Vulkan pipeline is deterministic in-process — same surface,
//     two consecutive frames, byte-exact equality on readback. A
//     failure here is a Vulkan render-pipeline non-determinism bug
//     (uninitialised descriptor sets, stale atlas state, …) — STOP
//     and escalate; do NOT relax the per-string tolerance.
//   * Raster ↔ Vulkan agree within tolerance. Looser than the
//     raster↔Metal probe because Vulkan ICDs (Mesa-llvmpipe, AMDVLK,
//     NVIDIA proprietary, lavapipe) vary in how they round subpixel
//     positions and AA coverage. The agreed-on tolerance for first
//     landing is **±20 %** on `opaque_pixels` and `darkness_sum`,
//     versus ±15 % for the raster↔Metal probe. Tighten only after
//     observing the actual digest spread on a real Vulkan runner.
//
// ────────────────────────────────────────────────────────────────
// What CI lane this needs:
//
//   * A Linux runner with a working Vulkan stack (`vulkan-loader` +
//     a real ICD, or lavapipe / llvmpipe as the software ICD).
//     `swiftshader-vk` is also acceptable as a deterministic
//     reference for headless CI.
//   * Skia built with Vulkan support. The currently-pinned macOS
//     arm64 Skia archive in `external/skia-build/` is Metal-only;
//     a Linux Skia archive with `skia_use_vulkan=true` (or the
//     Graphite-Vulkan backend wired in) is required.
//   * Dawn built with the Vulkan backend (`-DDAWN_ENABLE_VULKAN=ON`)
//     OR a direct Skia-Vulkan path (`GrVkBackendContext` /
//     `GrDirectContexts::MakeVulkan`) — both are acceptable, this
//     scaffold doesn't pick the route yet.
//   * A Windows runner is also acceptable for the Vulkan lane (Windows
//     supports Vulkan natively through the GPU vendor's driver) — the
//     compile guard intentionally accepts both `__linux__` and `_WIN32`.
//     The D3D-only lane is in `test_font_rendering_goldens_d3d.cpp`.
//
// ────────────────────────────────────────────────────────────────
// Why this lane is currently gated off:
//
//   * Pulp's required CI gate is the local self-hosted macOS-arm64
//     runner. That runner is Metal-only (see `test_font_rendering_
//     goldens_gpu.cpp` for the macOS-arm64 Metal lane that DOES run
//     today). There is no Vulkan-capable runner in the required
//     gate set.
//   * The GitHub-hosted Linux runner is advisory, not required, and
//     does not currently have Skia-with-Vulkan linked.
//   * Adding a Vulkan runner is tracked separately (Slice 3.4
//     follow-up work). Until that lane exists, building this TU
//     against Vulkan symbols would only burn CI roundtrips on
//     "couldn't find <vulkan/vulkan.h>" failures.
//
// ────────────────────────────────────────────────────────────────
// How to light this lane up (future work):
//
//   1. Add a Linux or Windows CI lane with Vulkan + Skia-Vulkan.
//   2. Set `-DPULP_VULKAN_AVAILABLE=ON` in that lane's CMake configure.
//   3. Land the implementation that replaces the placeholder TEST_CASE
//      below with real `GpuFixture` / `gpu_render_text` helpers shaped
//      after the Metal lane in `test_font_rendering_goldens_gpu.cpp`.
//   4. Commit observed-actual digests as the new constants
//      (`kVkHelloInter14`, …) once the first green run lands.
//
// Tag: [golden][gpu][vulkan][skia][font][issue-2257-followup][scaffold]

#include <catch2/catch_test_macros.hpp>

#if defined(PULP_HAS_SKIA) && (defined(__linux__) || defined(_WIN32)) && defined(PULP_VULKAN_AVAILABLE)

// Real Vulkan goldens land here once a Vulkan CI lane exists. The
// shape mirrors `test_font_rendering_goldens_gpu.cpp` (Metal): three
// per-string TEST_CASEs, a determinism TEST_CASE, a cross-backend
// raster↔Vulkan tolerance TEST_CASE. Tolerance for the cross-backend
// probe is ±20 % (see header rationale).

TEST_CASE("font v2 Slice 3.4 — Vulkan goldens scaffold placeholder",
          "[golden][gpu][vulkan][skia][font][scaffold][issue-2257-followup]") {
    // Placeholder. The real implementation imports Skia's Vulkan
    // context (`GrVkBackendContext` / `GrDirectContexts::MakeVulkan`
    // or Graphite-Vulkan via Dawn), allocates a 128×32 RGBA8 surface,
    // and runs the same three goldens as the Metal lane.
    //
    // This SUCCEED() exists only so the test target is non-empty when
    // PULP_VULKAN_AVAILABLE is ON but no Vulkan runner is wired yet.
    SUCCEED("Vulkan goldens scaffold — implementation lands when a "
            "Vulkan CI runner exists.");
}

#else  // !(PULP_HAS_SKIA && (linux||win) && PULP_VULKAN_AVAILABLE)

// No-op on every current Pulp CI lane:
//   • macOS-arm64 (Skia present but not Vulkan-capable → guard fails
//     on the `__linux__ || _WIN32` clause).
//   • Namespace macOS overflow (Skia not linked → guard fails on
//     PULP_HAS_SKIA).
//   • GitHub-hosted Linux/Windows (Skia not linked AND
//     PULP_VULKAN_AVAILABLE is OFF by default → guard fails on both).
//
// Intentionally no TEST_CASE here. The CMake side gates the target
// out entirely when PULP_VULKAN_AVAILABLE=OFF, so this TU is only
// compiled when the option is flipped ON by a future CI lane. Even
// then, if Skia or the platform doesn't match, the TU compiles to an
// empty object — no soft-skip TEST_CASE needed, because the target
// itself is conditional on PULP_VULKAN_AVAILABLE at CMake time and
// PULP_HAS_SKIA at compile time. Keeping the TU empty avoids
// polluting `ctest --output-on-failure` runs on lanes that legitimately
// can't run Vulkan with "Vulkan unavailable" SUCCEED noise.

#endif  // PULP_HAS_SKIA && (__linux__ || _WIN32) && PULP_VULKAN_AVAILABLE
