#pragma once

// Compile WGSL on a Dawn device and collect the compiler's messages.
//
// Shared by GpuDevice::create_shader_module() and the build-time validator
// (tools/wgsl/pulp_wgsl_validate.cpp), which runs it on Dawn's Null adapter:
// the Null backend draws nothing, but shader-module creation still runs the
// full WGSL front end, so the diagnostics are the ones a real device gives.

#include <pulp/render/wgsl_diagnostics.hpp>

#include "webgpu/webgpu_cpp.h"

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::render::detail {

struct WgslCompileOutput {
    /// Null when the source failed to compile.
    wgpu::ShaderModule module;
    std::vector<WgslDiagnostic> diagnostics;
    /// The validation error scope's message when the module was rejected
    /// without a positioned compiler message.
    std::string scope_error;
    bool timed_out = false;

    bool ok() const { return module && !timed_out && !has_wgsl_errors(diagnostics) && scope_error.empty(); }
};

/// Create a shader module under a validation error scope and wait (spinning
/// `instance`'s event loop, up to `timeout`) for its compilation info and the
/// scope result. Not real-time safe.
WgslCompileOutput compile_wgsl(const wgpu::Device& device, const wgpu::Instance& instance,
                               std::string_view code, std::string_view label,
                               std::chrono::milliseconds timeout);

} // namespace pulp::render::detail
