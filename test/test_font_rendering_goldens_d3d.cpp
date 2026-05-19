// test_font_rendering_goldens_d3d.cpp — Font v2 Slice 3.4
// (Cross-backend rendering goldens — Skia GPU Direct3D 12 lane, SCAFFOLD).
//
// Sibling of `test_font_rendering_goldens.cpp` (Skia raster),
// `test_font_rendering_goldens_gpu.cpp` (Skia GPU on Dawn → Metal,
// macOS-arm64), and `test_font_rendering_goldens_vulkan.cpp` (Skia
// GPU Vulkan, future Linux/Windows lane). This file is the
// **structural scaffold** for the Skia GPU Direct3D 12 lane. It does
// NOT wire up D3D at runtime yet — the CMake side gates the target
// out by default (PULP_D3D_AVAILABLE=OFF) and the compile-time guards
// below ensure the TU is a no-op on every CI lane Pulp currently runs.
//
// ────────────────────────────────────────────────────────────────
// What this lane would prove (once a Windows D3D runner exists):
//
//   * The Skia GPU backend on D3D12 (Graphite-D3D / GrD3DBackend,
//     depending on Skia point release) produces text rasters in the
//     same neighbourhood as the raster, Metal, and Vulkan lanes for
//     the three Slice 3.4 reference strings (`"Hello"` / Inter / 14 px,
//     `"日本語"` / Inter / 14 px CJK fallback, `"Hello world"` /
//     JetBrains Mono / 12 px).
//   * The D3D12 pipeline is deterministic in-process — same surface,
//     two consecutive frames, byte-exact equality on readback. A
//     failure here is a D3D render-pipeline non-determinism bug
//     (uninitialised descriptor heaps, stale resource state,
//     PSO cache fallout, …) — STOP and escalate; do NOT relax the
//     per-string tolerance.
//   * Raster ↔ D3D agree within tolerance. Looser than the
//     raster↔Metal probe because the WARP software adapter (the
//     deterministic CI choice on Windows) and the real GPU drivers
//     (NVIDIA, AMD, Intel) differ on subpixel positioning and AA
//     coverage. The agreed-on tolerance for first landing is **±20 %**
//     on `opaque_pixels` and `darkness_sum`, versus ±15 % for the
//     raster↔Metal probe. Tighten only after observing the actual
//     digest spread on a real D3D runner.
//
// ────────────────────────────────────────────────────────────────
// What CI lane this needs:
//
//   * A Windows runner. D3D12 is Windows-only — the compile guard
//     enforces `_WIN32`. For headless CI determinism, prefer the
//     WARP software adapter (`IDXGIFactory4::EnumWarpAdapter`)
//     which gives bit-for-bit identical output across runner hosts.
//   * Skia built with D3D support. The currently-pinned macOS arm64
//     Skia archive in `external/skia-build/` is Metal-only;
//     a Windows Skia archive with `skia_use_direct3d=true` (or the
//     Graphite-D3D backend wired in) is required.
//   * Either Dawn built with the D3D12 backend
//     (`-DDAWN_ENABLE_D3D12=ON`) OR a direct Skia-D3D path
//     (`GrD3DBackendContext` / `GrDirectContexts::MakeDirect3D`) —
//     this scaffold doesn't pick the route yet.
//
// ────────────────────────────────────────────────────────────────
// Why this lane is currently gated off:
//
//   * Pulp's required CI gate is the local self-hosted macOS-arm64
//     runner. There is no Windows runner in the required gate set.
//   * The GitHub-hosted Windows runner is advisory, not required, and
//     does not currently have Skia-with-D3D linked.
//   * Adding a Windows D3D runner is tracked separately (Slice 3.4
//     follow-up work). Until that lane exists, building this TU
//     against D3D symbols would only burn CI roundtrips on
//     "couldn't find <d3d12.h>" failures.
//
// ────────────────────────────────────────────────────────────────
// How to light this lane up (future work):
//
//   1. Add a Windows CI lane with D3D12 + Skia-D3D.
//   2. Set `-DPULP_D3D_AVAILABLE=ON` in that lane's CMake configure.
//   3. Land the implementation that replaces the placeholder
//      TEST_CASE below with real `GpuFixture` / `gpu_render_text`
//      helpers shaped after the Metal lane in
//      `test_font_rendering_goldens_gpu.cpp`.
//   4. Commit observed-actual digests as the new constants
//      (`kD3DHelloInter14`, …) once the first green run lands.
//      Prefer the WARP adapter for the committed values — real-GPU
//      digests are runner-dependent.
//
// Tag: [golden][gpu][d3d][skia][font][issue-2257-followup][scaffold]

#include <catch2/catch_test_macros.hpp>

#if defined(PULP_HAS_SKIA) && defined(_WIN32) && defined(PULP_D3D_AVAILABLE)

// Real D3D12 goldens land here once a Windows D3D CI lane exists. The
// shape mirrors `test_font_rendering_goldens_gpu.cpp` (Metal): three
// per-string TEST_CASEs, a determinism TEST_CASE, a cross-backend
// raster↔D3D tolerance TEST_CASE. Tolerance for the cross-backend
// probe is ±20 % (see header rationale).

TEST_CASE("font v2 Slice 3.4 — D3D12 goldens scaffold placeholder",
          "[golden][gpu][d3d][skia][font][scaffold][issue-2257-followup]") {
    // Placeholder. The real implementation imports Skia's D3D
    // context (`GrD3DBackendContext` / `GrDirectContexts::MakeDirect3D`
    // or Graphite-D3D via Dawn), allocates a 128×32 RGBA8 surface,
    // and runs the same three goldens as the Metal lane.
    //
    // This SUCCEED() exists only so the test target is non-empty when
    // PULP_D3D_AVAILABLE is ON but no D3D runner is wired yet.
    SUCCEED("D3D12 goldens scaffold — implementation lands when a "
            "Windows D3D CI runner exists.");
}

#else  // !(PULP_HAS_SKIA && _WIN32 && PULP_D3D_AVAILABLE)

// No-op on every current Pulp CI lane:
//   • macOS-arm64 (not Windows → guard fails on `_WIN32`).
//   • Namespace macOS overflow (not Windows AND Skia not linked).
//   • GitHub-hosted Linux (not Windows → guard fails on `_WIN32`).
//   • GitHub-hosted Windows (PULP_D3D_AVAILABLE is OFF by default →
//     guard fails on PULP_D3D_AVAILABLE).
//
// Intentionally no TEST_CASE here. The CMake side gates the target
// out entirely when PULP_D3D_AVAILABLE=OFF, so this TU is only
// compiled when the option is flipped ON by a future CI lane. Even
// then, if Skia or the platform doesn't match, the TU compiles to an
// empty object — no soft-skip TEST_CASE needed, because the target
// itself is conditional on PULP_D3D_AVAILABLE at CMake time and
// (_WIN32 + PULP_HAS_SKIA) at compile time.

#endif  // PULP_HAS_SKIA && _WIN32 && PULP_D3D_AVAILABLE
