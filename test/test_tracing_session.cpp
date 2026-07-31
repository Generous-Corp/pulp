// Tracing session lifecycle + macro smoke test.
//
// Config-agnostic: with PULP_TRACING=OFF (default/CI) it verifies the no-op
// contract (start() fails, nothing active). With PULP_TRACING=ON it drives the
// real process session — emits spans from two threads through the public macros,
// stops, and asserts the flushed .pftrace contains the interned span names (the
// byte-token check from the D3 spike, no trace_processor needed).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

#include <pulp/runtime/trace.hpp>
#include <pulp/runtime/trace_session.hpp>

using pulp::runtime::Tracing;

namespace {

void set_environment(
    const char* name,
    const std::optional<std::string>& value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value->c_str() : "");
#else
    if (value)
        setenv(name, value->c_str(), 1);
    else
        unsetenv(name);
#endif
}

class ScopedEnvironment {
public:
    ScopedEnvironment(
        const char* name,
        std::optional<std::string> value)
        : name_(name),
          previous_(std::getenv(name)
              ? std::optional<std::string>(std::getenv(name))
              : std::nullopt) {
        set_environment(name_.c_str(), value);
    }

    ~ScopedEnvironment() {
        set_environment(name_.c_str(), previous_);
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

} // namespace

TEST_CASE("tracing session lifecycle", "[tracing]") {
    if (!pulp::runtime::kTracingEnabled) {
        // OFF contract: every call is an inert no-op.
        REQUIRE_FALSE(Tracing::start());
        REQUIRE_FALSE(Tracing::active());
        REQUIRE_FALSE(Tracing::stop().ok);
        return;
    }

    // ON: capture two threads' spans into a real trace.
    auto out = std::filesystem::temp_directory_path() / "pulp-trace-smoke.pftrace";
    std::error_code ec;
    std::filesystem::remove(out, ec);

    REQUIRE(Tracing::start(/*categories=*/{}, out.string(), /*ring_kb=*/4096));
    REQUIRE(Tracing::active());
    // The public API remains idempotent, while exclusive callers can tell that
    // their replacement configuration was not applied.
    REQUIRE(Tracing::start());
    REQUIRE(Tracing::start_exclusive().status ==
            pulp::runtime::TraceStartStatus::AlreadyActive);
    const auto unowned = Tracing::ownership_status(nullptr);
    CHECK(unowned.active);
    CHECK_FALSE(unowned.owned);

    std::thread a([] {
        for (int i = 0; i < 500; ++i) {
            PULP_TRACE_SCOPE_NAMED("render", "frame_a");
        }
    });
    std::thread b([] {
        for (int i = 0; i < 500; ++i) {
            PULP_TRACE_SCOPE_NAMED("dsp", "block_b");
            PULP_TRACE_COUNTER("dsp", "load", i & 63);
        }
    });
    // Exercise the dynamic-name opt-in: a span name built at runtime must reach
    // Perfetto intact (DynamicString copies the bytes per event).
    {
        const std::string dyn_name = std::string("node_") + std::to_string(7 * 6);
        PULP_TRACE_SCOPE_DYNAMIC("dsp.node", dyn_name);
    }
    // Exercise the auto-named (prettifier) path too — must compile + run.
    { PULP_TRACE_SCOPE("state"); }
    a.join();
    b.join();

    auto r = Tracing::stop();
    REQUIRE(r.ok);
    REQUIRE(r.trace_bytes > 0);
    REQUIRE_FALSE(Tracing::active());

    std::ifstream f(out, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    // Interned span names are stored as UTF-8 in the trace stream.
    REQUIRE(bytes.find("frame_a") != std::string::npos);
    REQUIRE(bytes.find("block_b") != std::string::npos);
    // The runtime-computed name survived as UTF-8, proving the DynamicString path.
    REQUIRE(bytes.find("node_42") != std::string::npos);
}

TEST_CASE("runtime detach stops only its attach-owned capture",
          "[tracing][ownership]") {
    if (!pulp::runtime::kTracingEnabled)
        return;

    // Attaching while another controller is active must not claim it.
    auto external = Tracing::start_exclusive();
    REQUIRE(external.status == pulp::runtime::TraceStartStatus::Started);
    REQUIRE(external.ownership.has_value());
    auto transferred = std::move(*external.ownership);
    CHECK_FALSE(Tracing::ownership_status(&*external.ownership).owned);
    CHECK_FALSE(Tracing::stop_owned(*external.ownership).ok);
    CHECK(Tracing::active());
    Tracing::attach();
    Tracing::detach();
    CHECK(Tracing::ownership_status(&transferred).owned);
    REQUIRE(Tracing::stop_owned(transferred).ok);

    // If an attach-owned capture ends and is replaced before final detach,
    // its stale ownership token must not stop the replacement.
    const auto path =
        std::filesystem::temp_directory_path() /
        "pulp-trace-attach-ownership.pftrace";
    ScopedEnvironment path_environment(
        "PULP_TRACE_PATH", path.string());
    ScopedEnvironment seconds_environment(
        "PULP_TRACE_SECONDS", std::nullopt);
    Tracing::attach();
    REQUIRE(Tracing::active());
    REQUIRE(Tracing::stop().ok);
    auto replacement = Tracing::start_exclusive();
    REQUIRE(replacement.status == pulp::runtime::TraceStartStatus::Started);
    REQUIRE(replacement.ownership.has_value());
    Tracing::detach();
    CHECK(Tracing::ownership_status(&*replacement.ownership).owned);
    REQUIRE(Tracing::stop_owned(*replacement.ownership).ok);
}

TEST_CASE("tracing honors explicit category selection",
          "[tracing][categories]") {
    if (!pulp::runtime::kTracingEnabled)
        return;

    const auto out = std::filesystem::temp_directory_path() /
                     "pulp-trace-category-filter.pftrace";
    std::error_code error;
    std::filesystem::remove(out, error);

    auto started = Tracing::start_exclusive(
        {"render"}, out.string(), /*ring_kb=*/4096);
    REQUIRE(started.status == pulp::runtime::TraceStartStatus::Started);
    REQUIRE(started.ownership.has_value());
    { PULP_TRACE_SCOPE_NAMED("render", "selected_render_event"); }
    { PULP_TRACE_SCOPE_NAMED("dsp", "excluded_dsp_event"); }

    const auto stopped = Tracing::stop_owned(*started.ownership);
    REQUIRE(stopped.ok);
    std::ifstream trace(out, std::ios::binary);
    const std::string bytes{
        std::istreambuf_iterator<char>(trace),
        std::istreambuf_iterator<char>()};
    CHECK(bytes.find("selected_render_event") != std::string::npos);
    CHECK(bytes.find("excluded_dsp_event") == std::string::npos);
}
