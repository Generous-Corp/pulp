// GPU-diagnostics sink: counter contract, message bounding, and the polite
// Skia log-handler install. No GPU device is created — every case here is about
// the sink's own observable state.
#include <catch2/catch_test_macros.hpp>

#include <pulp/render/gpu_diagnostics.hpp>
#include <pulp/runtime/trace.hpp>

#include <cstdarg>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>

#if defined(PULP_HAS_SKIA) && PULP_HAS_SKIA && __has_include("include/utils/SkLogHandler.h")
#define PULP_TEST_HAS_SK_LOG_HANDLER 1
#include "include/utils/SkLogHandler.h"
#else
#define PULP_TEST_HAS_SK_LOG_HANDLER 0
#endif

using namespace pulp::render;

namespace {

void set_opt_in(const char* value) {
    if (value == nullptr) {
        ::unsetenv("PULP_GPU_LOG_BRIDGE");
    } else {
        ::setenv("PULP_GPU_LOG_BRIDGE", value, 1);
    }
}

} // namespace

TEST_CASE("GPU diagnostic severities carry distinct stable labels", "[gpu][diagnostics]") {
    std::set<std::string> labels;
    for (auto severity : {GpuDiagnosticSeverity::info, GpuDiagnosticSeverity::warning,
                          GpuDiagnosticSeverity::error, GpuDiagnosticSeverity::fatal}) {
        const std::string label = to_string(severity);
        REQUIRE_FALSE(label.empty());
        REQUIRE(label != "unknown");
        labels.insert(label);
    }
    REQUIRE(labels.size() == 4);
}

TEST_CASE("Skia log-bridge statuses carry distinct stable labels", "[gpu][diagnostics]") {
    std::set<std::string> labels;
    for (auto status :
         {SkiaLogBridgeStatus::not_attempted, SkiaLogBridgeStatus::installed,
          SkiaLogBridgeStatus::declined_handler_present, SkiaLogBridgeStatus::declined_not_enabled,
          SkiaLogBridgeStatus::unavailable_no_skia}) {
        const std::string label = to_string(status);
        REQUIRE_FALSE(label.empty());
        REQUIRE(label != "unknown");
        labels.insert(label);
    }
    REQUIRE(labels.size() == 5);
}

TEST_CASE("emit_gpu_diagnostic counts every record it is handed", "[gpu][diagnostics]") {
    const auto before = gpu_diagnostics_stats();

    emit_gpu_diagnostic(GpuDiagnosticSeverity::error, "dawn.uncaptured_error",
                        "Validation error: buffer is destroyed");
    emit_gpu_diagnostic(GpuDiagnosticSeverity::fatal, "dawn.device_lost", "device lost");

    const auto after = gpu_diagnostics_stats();
    REQUIRE(after.emitted == before.emitted + 2);

    // `traced` is what separates "the call site never ran" from "the call site
    // ran in a build that compiles the trace macros out".
    if constexpr (pulp::runtime::kTracingEnabled) {
        REQUIRE(after.traced == before.traced + 2);
    } else {
        REQUIRE(after.traced == before.traced);
        REQUIRE(after.traced == 0);
    }
}

TEST_CASE("emit_gpu_diagnostic accepts a null source and an empty message", "[gpu][diagnostics]") {
    const auto before = gpu_diagnostics_stats();
    emit_gpu_diagnostic(GpuDiagnosticSeverity::info, nullptr, std::string_view{});
    const auto after = gpu_diagnostics_stats();
    REQUIRE(after.emitted == before.emitted + 1);
    REQUIRE(after.truncated == before.truncated);
}

TEST_CASE("emit_gpu_diagnostic bounds an oversized message instead of allocating",
          "[gpu][diagnostics]") {
    const std::string fits(kGpuDiagnosticMessageLimit, 'a');
    const std::string overflows(kGpuDiagnosticMessageLimit + 1, 'b');

    auto before = gpu_diagnostics_stats();
    emit_gpu_diagnostic(GpuDiagnosticSeverity::warning, "dawn.uncaptured_error", fits);
    auto after = gpu_diagnostics_stats();
    REQUIRE(after.truncated == before.truncated);

    before = after;
    emit_gpu_diagnostic(GpuDiagnosticSeverity::warning, "dawn.uncaptured_error", overflows);
    after = gpu_diagnostics_stats();
    REQUIRE(after.truncated == before.truncated + 1);
    REQUIRE(after.emitted == before.emitted + 1);
}

// A build-configuration parity check, not a behavioural one: this translation
// unit and the library must agree about whether the Skia handler was compiled
// in. It fails only when the two disagree — a test target that lost
// PULP_HAS_SKIA while the library kept it would report a bridge that callers
// here cannot reach. It deliberately proves nothing about what the bridge does;
// the install and forwarding cases below carry that.
TEST_CASE("the Skia log bridge agrees with this build about its own presence",
          "[gpu][diagnostics]") {
#if PULP_TEST_HAS_SK_LOG_HANDLER
    REQUIRE(skia_log_bridge_available());
#else
    REQUIRE_FALSE(skia_log_bridge_available());
#endif
}

TEST_CASE("the Skia log-bridge opt-in follows the environment", "[gpu][diagnostics]") {
    set_opt_in("1");
    REQUIRE(skia_log_bridge_enabled());
    set_opt_in("true");
    REQUIRE(skia_log_bridge_enabled());

    // An explicit off wins even in a tracing build, so a tracing run can still
    // leave a host's Skia logging alone.
    set_opt_in("0");
    REQUIRE_FALSE(skia_log_bridge_enabled());
    set_opt_in("off");
    REQUIRE_FALSE(skia_log_bridge_enabled());

    // An unparseable value is not an opt-in signal; the build default stands.
    set_opt_in("maybe");
    REQUIRE(skia_log_bridge_enabled() == pulp::runtime::kTracingEnabled);

    set_opt_in(nullptr);
    REQUIRE(skia_log_bridge_enabled() == pulp::runtime::kTracingEnabled);
}

TEST_CASE("an absent opt-in declines without installing", "[gpu][diagnostics]") {
    set_opt_in("0");
    const auto before = skia_log_bridge_state();
    const auto status = install_skia_log_bridge_if_enabled();

    if (before.status == SkiaLogBridgeStatus::not_attempted) {
        REQUIRE(status == SkiaLogBridgeStatus::declined_not_enabled);
        REQUIRE(skia_log_bridge_state().status == SkiaLogBridgeStatus::declined_not_enabled);
    }
    // A declined opt-in can never be the thing that installs a handler.
    REQUIRE((status != SkiaLogBridgeStatus::installed ||
             before.status == SkiaLogBridgeStatus::installed));
    set_opt_in(nullptr);
}

TEST_CASE("an explicit install reports a terminal, idempotent outcome", "[gpu][diagnostics]") {
    const auto first = install_skia_log_bridge();

    // "Not attempted" and "not enabled" are both non-answers; an explicit
    // install must never leave the bridge in one.
    REQUIRE(first != SkiaLogBridgeStatus::not_attempted);
    REQUIRE(first != SkiaLogBridgeStatus::declined_not_enabled);

    if (skia_log_bridge_available()) {
        REQUIRE((first == SkiaLogBridgeStatus::installed ||
                 first == SkiaLogBridgeStatus::declined_handler_present));
    } else {
        REQUIRE(first == SkiaLogBridgeStatus::unavailable_no_skia);
    }

    REQUIRE(install_skia_log_bridge() == first);
    REQUIRE(skia_log_bridge_state().status == first);
}

#if PULP_TEST_HAS_SK_LOG_HANDLER
namespace {

void log_through_skia(SkLogHandler& handler, SkLogPriority priority, const char* format, ...) {
    va_list args;
    va_start(args, format);
    handler.onLog(priority, format, args);
    va_end(args);
}

} // namespace

TEST_CASE("an installed Skia handler forwards records into the same sink", "[gpu][diagnostics]") {
    if (install_skia_log_bridge() != SkiaLogBridgeStatus::installed) {
        SUCCEED("another SkLogHandler owns this process; the bridge correctly declined");
        return;
    }

    auto handler = SkLogHandler::GetInstance();
    REQUIRE(handler != nullptr);

    const auto sink_before = gpu_diagnostics_stats();
    const auto bridge_before = skia_log_bridge_state();

    log_through_skia(*handler, SkLogPriority::kError, "graphite: %s failed (%d)", "submit", 7);

    const auto bridge_after = skia_log_bridge_state();
    const auto sink_after = gpu_diagnostics_stats();
    REQUIRE(bridge_after.records_forwarded == bridge_before.records_forwarded + 1);
    REQUIRE(sink_after.emitted == sink_before.emitted + 1);
}
#endif // PULP_TEST_HAS_SK_LOG_HANDLER
