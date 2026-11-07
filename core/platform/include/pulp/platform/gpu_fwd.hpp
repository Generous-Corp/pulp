#pragma once

// gpu_fwd.hpp — the one place the GPU device types are forward-declared.
//
// Where the device layer is gpu-runtime's (PULP_GPU_RUNTIME, set on
// pulp::platform by tools/cmake/PulpGpuRuntime.cmake), pulp::render::GpuSurface
// names gpu_runtime::GpuSurface, so a hand-written `class GpuSurface;` in
// pulp::render would declare a second, unrelated class. Headers that need these
// names without their definitions include this instead; it depends on nothing,
// so it lives where every consumer already looks. core_vellum_direction.py
// refuses a raw forward declaration of them anywhere else.

#if defined(PULP_GPU_RUNTIME)
namespace gpu_runtime {
class GpuSurface;
class GpuCompute;
namespace bench {
struct PerfCounters;
} // namespace bench
} // namespace gpu_runtime

namespace pulp::render {
using gpu_runtime::GpuCompute;
using gpu_runtime::GpuSurface;
} // namespace pulp::render

namespace pulp::render::bench {
using gpu_runtime::bench::PerfCounters;
} // namespace pulp::render::bench
#else
namespace pulp::render {
class GpuSurface;
class GpuCompute;
} // namespace pulp::render

namespace pulp::render::bench {
struct PerfCounters;
} // namespace pulp::render::bench
#endif
