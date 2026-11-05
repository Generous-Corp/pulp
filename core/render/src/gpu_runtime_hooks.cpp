// Connects gpu-runtime's host hooks to Pulp, for builds whose device layer is
// gpu-runtime (PULP_GPU_RUNTIME, see tools/cmake/PulpGpuRuntime.cmake).
//
// gpu-runtime depends on no framework, so the host supplies what Pulp's own
// device sources used directly:
//   * its log lines go to pulp::runtime's log;
//   * its trace events become Pulp's Perfetto events (a tracing build only, so
//     GpuDiagnosticsStats::traced keeps meaning "reached Perfetto");
//   * it reads Pulp's environment variables (PULP_GPU_LOG_BRIDGE,
//     PULP_GPU_SOFTWARE_ADAPTER) at the moment of use, as before;
//   * the Skia log bridge is registered, since this library links Skia.
//
// The hooks install from a static initializer. Nothing references this file's
// code otherwise, so pulp-gpu-device's link interface names its anchor symbol
// (-u), which makes every image that links the device layer load it.

#include <gpu_runtime/gpu_diagnostics.hpp>
#include <gpu_runtime/gpu_host_hooks.hpp>
#include <gpu_runtime/skia_log_bridge.hpp>

#include <pulp/runtime/log.hpp>
#include <pulp/runtime/trace.hpp>

#include <cstddef>
#include <cstring>
#include <string_view>

namespace pulp::render::detail {
namespace {

void log_line(gpu_runtime::LogLevel level, std::string_view message, void*) {
    switch (level) {
    case gpu_runtime::LogLevel::info:
        runtime::log_info("{}", message);
        break;
    case gpu_runtime::LogLevel::warning:
        runtime::log_warn("{}", message);
        break;
    case gpu_runtime::LogLevel::error:
        runtime::log_error("{}", message);
        break;
    }
}

[[maybe_unused]] const char* arg(const gpu_runtime::TraceArg* args, std::size_t count, const char* key) {
    for (std::size_t index = 0; index < count; ++index) {
        if (args[index].key != nullptr && std::strcmp(args[index].key, key) == 0)
            return args[index].value != nullptr ? args[index].value : "";
    }
    return "";
}

// gpu-runtime emits category "gpu" only; Perfetto needs it at compile time.
[[maybe_unused]] void trace_instant(const char*, const char* name, const gpu_runtime::TraceArg* args,
                   std::size_t count, void*) {
    if (std::strcmp(name, "gpu.diagnostic") == 0) {
        PULP_TRACE_INSTANT_ARGS("gpu", "gpu.diagnostic", "severity", arg(args, count, "severity"),
                                "source", arg(args, count, "source"), "message",
                                arg(args, count, "message"));
    } else {
        PULP_TRACE_INSTANT_DYNAMIC("gpu", name);
    }
}

[[maybe_unused]] void trace_begin(const char*, const char* name, void*) {
    PULP_TRACE_BEGIN_DYNAMIC("gpu", name);
}

[[maybe_unused]] void trace_end(const char*, void*) {
    PULP_TRACE_END("gpu");
}

struct InstallHooks {
    InstallHooks() {
        gpu_runtime::set_log_sink(&log_line, nullptr);
        if constexpr (runtime::kTracingEnabled) {
            gpu_runtime::set_trace_sink({.instant = &trace_instant,
                                         .begin = &trace_begin,
                                         .end = &trace_end,
                                         .context = nullptr});
        }
        gpu_runtime::RuntimeOptions options;
        options.environment_prefix = "PULP_GPU_";
        gpu_runtime::set_runtime_options(options);
        (void)gpu_runtime::register_skia_log_bridge(gpu_runtime::skia_log_bridge_installer());
    }
};

const InstallHooks install_hooks;

} // namespace
} // namespace pulp::render::detail

// Referenced by name from pulp-gpu-device's link interface so the linker loads
// this object, and with it the initializer above, into every consumer.
extern "C" const void* pulp_gpu_runtime_hooks_anchor() {
    return &pulp::render::detail::install_hooks;
}
