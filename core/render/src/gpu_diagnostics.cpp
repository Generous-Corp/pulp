#include <pulp/render/gpu_diagnostics.hpp>

#include <pulp/runtime/trace.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

// Skia's log-handler interface arrived in the m153 slice. The header check is
// belt-and-braces beside PULP_HAS_SKIA: a headers-only worktree can define the
// macro while the include tree predates SkLogHandler, and that must degrade to
// `unavailable_no_skia` rather than a build break.
#if defined(PULP_HAS_SKIA) && PULP_HAS_SKIA && __has_include("include/utils/SkLogHandler.h")
#define PULP_RENDER_HAS_SK_LOG_HANDLER 1
#include "include/core/SkRefCnt.h"
#include "include/utils/SkLogHandler.h"
#else
#define PULP_RENDER_HAS_SK_LOG_HANDLER 0
#endif

namespace pulp::render {

namespace {

std::atomic<std::uint64_t> g_emitted{0};
std::atomic<std::uint64_t> g_traced{0};
std::atomic<std::uint64_t> g_truncated{0};

std::atomic<SkiaLogBridgeStatus> g_bridge_status{SkiaLogBridgeStatus::not_attempted};
std::atomic<std::uint64_t> g_bridge_records{0};
std::mutex g_bridge_mutex;

bool env_flag(const char* name, bool& value) noexcept {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0')
        return false;
    const std::string_view text(raw);
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        value = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        value = false;
        return true;
    }
    return false;
}

} // namespace

const char* to_string(GpuDiagnosticSeverity severity) noexcept {
    switch (severity) {
    case GpuDiagnosticSeverity::info:
        return "info";
    case GpuDiagnosticSeverity::warning:
        return "warning";
    case GpuDiagnosticSeverity::error:
        return "error";
    case GpuDiagnosticSeverity::fatal:
        return "fatal";
    }
    return "unknown";
}

const char* to_string(SkiaLogBridgeStatus status) noexcept {
    switch (status) {
    case SkiaLogBridgeStatus::not_attempted:
        return "not_attempted";
    case SkiaLogBridgeStatus::installed:
        return "installed";
    case SkiaLogBridgeStatus::declined_handler_present:
        return "declined_handler_present";
    case SkiaLogBridgeStatus::declined_not_enabled:
        return "declined_not_enabled";
    case SkiaLogBridgeStatus::unavailable_no_skia:
        return "unavailable_no_skia";
    }
    return "unknown";
}

void emit_gpu_diagnostic(GpuDiagnosticSeverity severity, const char* source,
                         std::string_view message) noexcept {
    g_emitted.fetch_add(1, std::memory_order_relaxed);

    // Bounded stack copy: the annotation needs a null-terminated buffer and the
    // render thread must not allocate to get one.
    const std::size_t copied = std::min(message.size(), kGpuDiagnosticMessageLimit);
    if (copied < message.size())
        g_truncated.fetch_add(1, std::memory_order_relaxed);

    std::array<char, kGpuDiagnosticMessageLimit + 1> buffer{};
    if (copied > 0)
        std::memcpy(buffer.data(), message.data(), copied);
    buffer[copied] = '\0';

    const char* const tag = (source != nullptr) ? source : "unknown";
    const char* const severity_label = to_string(severity);

    // One interned event name for every GPU diagnostic; severity and source are
    // low-cardinality annotations, so `GROUP BY` on the slice name still works
    // and the variable text rides along as debug.message.
    PULP_TRACE_INSTANT_ARGS("gpu", "gpu.diagnostic", "severity", severity_label, "source", tag,
                            "message", static_cast<const char*>(buffer.data()));

    if constexpr (runtime::kTracingEnabled) {
        g_traced.fetch_add(1, std::memory_order_relaxed);
    } else {
        (void)tag;
        (void)severity_label;
    }
}

GpuDiagnosticsStats gpu_diagnostics_stats() noexcept {
    GpuDiagnosticsStats stats;
    stats.emitted = g_emitted.load(std::memory_order_relaxed);
    stats.traced = g_traced.load(std::memory_order_relaxed);
    stats.truncated = g_truncated.load(std::memory_order_relaxed);
    return stats;
}

bool skia_log_bridge_available() noexcept {
    return PULP_RENDER_HAS_SK_LOG_HANDLER != 0;
}

bool skia_log_bridge_enabled() noexcept {
    bool value = runtime::kTracingEnabled;
    env_flag("PULP_GPU_LOG_BRIDGE", value);
    return value;
}

SkiaLogBridgeState skia_log_bridge_state() noexcept {
    SkiaLogBridgeState state;
    state.status = g_bridge_status.load(std::memory_order_relaxed);
    state.records_forwarded = g_bridge_records.load(std::memory_order_relaxed);
    return state;
}

#if PULP_RENDER_HAS_SK_LOG_HANDLER

namespace {

GpuDiagnosticSeverity severity_of(SkLogPriority priority) noexcept {
    switch (priority) {
    case SkLogPriority::kError:
        return GpuDiagnosticSeverity::error;
    case SkLogPriority::kWarning:
        return GpuDiagnosticSeverity::warning;
    case SkLogPriority::kInfo:
        return GpuDiagnosticSeverity::info;
    case SkLogPriority::kDebug:
        return GpuDiagnosticSeverity::info;
    }
    return GpuDiagnosticSeverity::info;
}

// Skia may log from any thread, so this holds no mutable state beyond the two
// relaxed counters the sink already owns.
class TraceLogHandler final : public SkLogHandler {
  public:
    void onLog(SkLogPriority priority, const char format[], va_list args) override {
        std::array<char, kGpuDiagnosticMessageLimit + 1> buffer{};
        int written = 0;
        if (format != nullptr) {
            written = std::vsnprintf(buffer.data(), buffer.size(), format, args);
        }
        if (written < 0)
            written = 0;
        const std::size_t length =
            std::min(static_cast<std::size_t>(written), kGpuDiagnosticMessageLimit);

        g_bridge_records.fetch_add(1, std::memory_order_relaxed);
        emit_gpu_diagnostic(severity_of(priority), "skia.log",
                            std::string_view(buffer.data(), length));
    }
};

} // namespace

SkiaLogBridgeStatus install_skia_log_bridge() noexcept {
    const std::lock_guard<std::mutex> lock(g_bridge_mutex);
    const auto current = g_bridge_status.load(std::memory_order_relaxed);
    if (current == SkiaLogBridgeStatus::installed ||
        current == SkiaLogBridgeStatus::declined_handler_present) {
        return current;
    }

    // Polite install: Skia's handler is process-global and first-install-wins,
    // and a host DAW may already own it. Read before writing, and treat an
    // occupied slot as a normal outcome — never displace, never retry.
    if (SkLogHandler::GetInstance() != nullptr) {
        g_bridge_status.store(SkiaLogBridgeStatus::declined_handler_present,
                              std::memory_order_relaxed);
        return SkiaLogBridgeStatus::declined_handler_present;
    }

    const bool ok = SkLogHandler::SetInstance(sk_make_sp<TraceLogHandler>());
    // A false return means somebody won the race between the read above and
    // this write. Their handler stands.
    const auto result =
        ok ? SkiaLogBridgeStatus::installed : SkiaLogBridgeStatus::declined_handler_present;
    g_bridge_status.store(result, std::memory_order_relaxed);
    return result;
}

#else // no Skia log handler in this build

SkiaLogBridgeStatus install_skia_log_bridge() noexcept {
    g_bridge_status.store(SkiaLogBridgeStatus::unavailable_no_skia, std::memory_order_relaxed);
    return SkiaLogBridgeStatus::unavailable_no_skia;
}

#endif // PULP_RENDER_HAS_SK_LOG_HANDLER

SkiaLogBridgeStatus install_skia_log_bridge_if_enabled() noexcept {
    if (!skia_log_bridge_enabled()) {
        const std::lock_guard<std::mutex> lock(g_bridge_mutex);
        const auto current = g_bridge_status.load(std::memory_order_relaxed);
        // A prior explicit install already reported a terminal outcome; an
        // absent opt-in must not overwrite it with a weaker one.
        if (current == SkiaLogBridgeStatus::not_attempted) {
            g_bridge_status.store(SkiaLogBridgeStatus::declined_not_enabled,
                                  std::memory_order_relaxed);
            return SkiaLogBridgeStatus::declined_not_enabled;
        }
        return current;
    }
    return install_skia_log_bridge();
}

} // namespace pulp::render
