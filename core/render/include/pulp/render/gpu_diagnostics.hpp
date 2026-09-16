// gpu_diagnostics.hpp — one sink for GPU-layer diagnostics.
//
// Dawn hands errors to a callback; Skia hands log records to an SkLogHandler.
// Both funnel through emit_gpu_diagnostic(), which publishes a `gpu`-category
// Perfetto instant event so a device error lands on the same timeline as the
// frame that provoked it. This ADDS a trace signal — every call site keeps its
// runtime::log_* call, because a log is what a user reads and a trace is what a
// timeline query joins against.
//
// RT-safety: the Dawn uncaptured-error callback fires on the render thread.
// emit_gpu_diagnostic() allocates nothing: the message is copied into a bounded
// stack buffer and attached to a compile-time event name as a debug annotation,
// so the event name stays interned and nothing reaches the heap. It is still a
// Perfetto TRACE_EVENT and therefore must never be placed on the audio thread's
// live process() path (see trace.hpp).
//
// Observability: a bridge that quietly emits nothing forever is indistinguishable
// from a quiet GPU. gpu_diagnostics_stats() and skia_log_bridge_state() exist so
// "nothing was wired up" is always distinguishable from "nothing went wrong".
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pulp::render {

/// Severity of one GPU diagnostic record.
enum class GpuDiagnosticSeverity {
    info,
    warning,
    error,
    fatal,
};

/// Stable lowercase label for a severity, suitable as a trace annotation value.
const char* to_string(GpuDiagnosticSeverity severity) noexcept;

/// Publish one GPU diagnostic as a `gpu`-category instant event.
///
/// `source` is a stable, low-cardinality tag naming the producer
/// ("dawn.uncaptured_error", "dawn.device_lost", "skia.log"). `message` is the
/// variable text; it is truncated to kGpuDiagnosticMessageLimit bytes rather
/// than allocated. Callers may pass a non-null-terminated view.
///
/// Counted unconditionally, including in a PULP_TRACING=OFF build, so the
/// counter proves the call site ran even where no Perfetto event exists.
void emit_gpu_diagnostic(GpuDiagnosticSeverity severity, const char* source,
                         std::string_view message) noexcept;

/// Longest message text carried into a trace annotation. Longer messages are
/// truncated; the full text still reaches the log.
inline constexpr std::size_t kGpuDiagnosticMessageLimit = 512;

/// Counters that separate "the sink was never called" from "the sink was called
/// but this build compiles the trace macros out".
struct GpuDiagnosticsStats {
    /// Records handed to emit_gpu_diagnostic() in this process.
    std::uint64_t emitted = 0;
    /// Records forwarded to Perfetto. Always 0 in a PULP_TRACING=OFF build.
    std::uint64_t traced = 0;
    /// Records whose message text was truncated to the limit above.
    std::uint64_t truncated = 0;
};

GpuDiagnosticsStats gpu_diagnostics_stats() noexcept;

/// Outcome of trying to route Skia's process-global log handler into the sink.
///
/// SkLogHandler::SetInstance is first-install-wins and process-global, and Pulp
/// loads into hosts that may use Skia themselves, so every outcome other than
/// `installed` is a normal, reportable state rather than an error.
enum class SkiaLogBridgeStatus {
    /// install_skia_log_bridge*() has not run yet.
    not_attempted,
    /// This process owns Skia's log handler and records are flowing.
    installed,
    /// Somebody else installed a handler first; theirs is left untouched.
    declined_handler_present,
    /// The opt-in was absent, so no install was attempted.
    declined_not_enabled,
    /// This build has no Skia, so there is no handler to install.
    unavailable_no_skia,
};

/// Stable lowercase label for a bridge status, suitable for status output.
const char* to_string(SkiaLogBridgeStatus status) noexcept;

struct SkiaLogBridgeState {
    SkiaLogBridgeStatus status = SkiaLogBridgeStatus::not_attempted;
    /// Skia log records forwarded into the sink. Zero with an `installed`
    /// status means Skia has logged nothing — a different fact from a status of
    /// `declined_handler_present`, where nothing could ever arrive.
    std::uint64_t records_forwarded = 0;
};

SkiaLogBridgeState skia_log_bridge_state() noexcept;

/// True when this build contains the Skia log-handler subclass at all. False
/// means `unavailable_no_skia` is the only outcome an install can ever report,
/// which is the difference between "nothing to install" and "declined to".
bool skia_log_bridge_available() noexcept;

/// True when the opt-in is satisfied: a PULP_TRACING=ON build, or
/// PULP_GPU_LOG_BRIDGE set to 1/true/yes/on. Setting PULP_GPU_LOG_BRIDGE to
/// 0/false/no/off wins over the tracing build, so a tracing build can still
/// leave the host's Skia logging alone.
bool skia_log_bridge_enabled() noexcept;

/// Install the bridge, honouring the opt-in. Without it the status becomes
/// `declined_not_enabled` and Skia is not touched. This is the entry point a
/// host or app calls; a plugin build ships with the opt-in off.
SkiaLogBridgeStatus install_skia_log_bridge_if_enabled() noexcept;

/// Install the bridge regardless of the opt-in — calling this IS the explicit
/// opt-in. Installs only when SkLogHandler::GetInstance() is null; an existing
/// handler is never displaced. Idempotent: repeat calls report the first
/// outcome.
SkiaLogBridgeStatus install_skia_log_bridge() noexcept;

} // namespace pulp::render
