#include <pulp/render/gpu_startup_report.hpp>

#include <pulp/runtime/log.hpp>

#include <format>
#include <string_view>

namespace pulp::render {

namespace {

// Quote a free-text field so it stays one token on one line: escape the quote
// and backslash, and flatten control characters (a driver string with a
// newline would otherwise forge a second log line).
std::string quoted(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::string ms_field(double ms) {
    if (!(ms >= 0.0))  // also rejects NaN
        return "n/a";
    return std::format("{:.1f}", ms);
}

} // namespace

const char* adapter_type_label(GpuSurface::AdapterType type) noexcept {
    switch (type) {
    case GpuSurface::AdapterType::integrated_gpu:
        return "integrated";
    case GpuSurface::AdapterType::discrete_gpu:
        return "discrete";
    case GpuSurface::AdapterType::cpu:
        return "cpu";
    case GpuSurface::AdapterType::unknown:
        return "unknown";
    }
    return "unknown";
}

std::string format_gpu_adapter_line(const GpuSurface::AdapterInfo& info) {
    return std::format(
        "GpuSurface: adapter name={} vendor={} architecture={} description={} "
        "type={} backend={} null={}",
        quoted(info.name), quoted(info.vendor), quoted(info.architecture),
        quoted(info.description), adapter_type_label(info.adapter_type),
        info.backend_type, info.null_backend ? "true" : "false");
}

std::string format_gpu_startup_line(const GpuStartupReport& report) {
    return std::format(
        "GpuSurface: startup_ms instance={} adapter={} device={} surface={} "
        "graphite={} first_frame={}",
        ms_field(report.surface.instance_ms), ms_field(report.surface.adapter_ms),
        ms_field(report.surface.device_ms), ms_field(report.surface.surface_ms),
        ms_field(report.graphite_ms), ms_field(report.first_frame_ms));
}

std::string format_gpu_diagnostics_line(const SkiaLogBridgeState& bridge,
                                        const GpuDiagnosticsStats& stats) {
    return std::format(
        "GpuDiagnostics: skia_bridge={} skia_records={} gpu_diagnostics_emitted={} "
        "truncated={} skia_text_suppressed={}",
        to_string(bridge.status), bridge.records_forwarded, stats.emitted,
        stats.truncated, bridge.text_suppressed);
}

void log_gpu_diagnostics_summary() {
    const auto bridge = skia_log_bridge_state();
    if (bridge.text_suppressed > 0)
        runtime::log_info("skia: {} more records suppressed", bridge.text_suppressed);
    runtime::log_info("{}", format_gpu_diagnostics_line(bridge, gpu_diagnostics_stats()));
}

} // namespace pulp::render
