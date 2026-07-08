// trace.hpp — Pulp tracing macros (Perfetto-backed, DEV ONLY, OFF by default).
//
// This is the ONLY header instrumentation code includes for tracing. With
// PULP_TRACING=OFF — the default and only shippable configuration — every macro
// compiles to nothing and NO Perfetto header is pulled in: zero cost, zero
// symbols (see tools/cmake/PulpTracing.cmake and the guards it names).
//
// RT-safety: D1 (planning/2026-07-08-perfetto-tracing-plan.md §0c) proved
// Perfetto's TRACE_EVENT is NOT real-time-safe — it locks a mutex on chunk
// rollover. These macros must NEVER be placed on the audio thread's live
// process() path. They are for UI / render / process-level and OFFLINE-audio
// spans only; live DSP observability uses the fixed-slot per-node telemetry
// fallback instead.
//
// The Perfetto-backed macro bodies, the category registry, and the session
// controller light up (ON only) with the tracing session; this header provides
// the stable no-op surface and the compile-time enabled flag those build on.
#pragma once

namespace pulp::runtime {

/// True only in a PULP_TRACING=ON build. Every shipping/default build is false.
/// The Catch2 guard in test/test_tracing.cpp asserts this is false by default.
#if defined(PULP_TRACING_ENABLED) && PULP_TRACING_ENABLED
inline constexpr bool kTracingEnabled = true;
#else
inline constexpr bool kTracingEnabled = false;
#endif

}  // namespace pulp::runtime

// Stable macro surface. Names exist as no-ops in every configuration so
// instrumentation sites can be written now; the Perfetto-backed bodies (ON only)
// arrive with the session controller. `category` is one of the fixed taxonomy
// strings (dsp, dsp.node, render, layout, canvas, text, js, gpu, state, io).
#define PULP_TRACE_SCOPE(category) ((void)0)
#define PULP_TRACE_SCOPE_NAMED(category, name) ((void)0)
#define PULP_TRACE_BEGIN(category, name) ((void)0)
#define PULP_TRACE_END(category) ((void)0)
#define PULP_TRACE_COUNTER(category, name, value) ((void)0)
