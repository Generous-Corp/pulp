// pulp-wgsl-validate: compile WGSL files with Dawn's WGSL front end.
//
//   pulp-wgsl-validate [--stamp <file>] <shader.wgsl>...
//
// Each file becomes a shader module on Dawn's Null adapter. The Null backend
// executes nothing, but module creation runs the full WGSL parser and
// validator, so its messages are the ones a real device reports. Messages
// print as `path:line:column: severity: message`.
//
// Exit status:
//   0   every file compiled (warnings are printed, not fatal)
//   1   a file failed to compile or could not be read
//   2   bad command line
//   77  no Null adapter could be created, so nothing was validated. With
//       --stamp (the build-time mode used by pulp_add_wgsl_shaders) this is a
//       printed warning and exit 0 instead, because a build must not depend
//       on a validation adapter; tests run without --stamp and report a skip.
//
// Built from the same compile helper GpuDevice::create_shader_module() uses
// (core/render/src/wgsl_compile.cpp). It links only the device layer, never
// pulp-render, so pulp-render's own shaders can be validated while it builds.

#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/wgsl_diagnostics.hpp>

#include "wgsl_compile.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kExitSkipped = 77;

bool read_file(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

bool write_stamp(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

} // namespace

int main(int argc, char** argv) {
    std::string stamp;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stamp" && i + 1 < argc) {
            stamp = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: pulp-wgsl-validate [--stamp <file>] <shader.wgsl>...\n";
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "pulp-wgsl-validate: unknown option " << arg << "\n";
            return 2;
        } else {
            files.push_back(arg);
        }
    }
    if (files.empty()) {
        std::cerr << "usage: pulp-wgsl-validate [--stamp <file>] <shader.wgsl>...\n";
        return 2;
    }

    auto surface = pulp::render::GpuSurface::create_dawn();
    pulp::render::GpuSurface::Config config;
    config.width = 1;
    config.height = 1;
    config.native_surface_handle = nullptr;
    config.backend_preference = pulp::render::GpuSurface::AdapterBackendPreference::null_backend;
    if (!surface || !surface->initialize(config) || surface->dawn_device_handle() == nullptr ||
        surface->dawn_instance_handle() == nullptr) {
        std::cerr << "pulp-wgsl-validate: warning: no Dawn Null adapter; WGSL was not validated\n";
        if (!stamp.empty())
            return write_stamp(stamp, "skipped: no Null adapter\n") ? 0 : 1;
        return kExitSkipped;
    }
    const auto& device = *static_cast<wgpu::Device*>(surface->dawn_device_handle());
    const auto& instance = *static_cast<wgpu::Instance*>(surface->dawn_instance_handle());

    bool ok = true;
    for (const auto& path : files) {
        std::string source;
        if (!read_file(path, source)) {
            std::cerr << path << ": error: cannot read file\n";
            ok = false;
            continue;
        }
        auto result = pulp::render::detail::compile_wgsl(device, instance, source, path,
                                                         std::chrono::milliseconds(10000));
        if (!result.diagnostics.empty())
            std::cerr << pulp::render::format_wgsl_diagnostics(path, result.diagnostics) << "\n";
        if (!result.scope_error.empty())
            std::cerr << path << ": error: " << result.scope_error << "\n";
        if (result.timed_out)
            std::cerr << path << ": error: compilation did not report back\n";
        if (!result.ok())
            ok = false;
    }
    if (!ok)
        return 1;
    if (!stamp.empty() && !write_stamp(stamp, "ok\n")) {
        std::cerr << "pulp-wgsl-validate: cannot write " << stamp << "\n";
        return 1;
    }
    return 0;
}
