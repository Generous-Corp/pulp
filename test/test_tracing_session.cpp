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
#include <string>
#include <thread>

#include <pulp/runtime/trace.hpp>
#include <pulp/runtime/trace_session.hpp>

using pulp::runtime::Tracing;

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
    // A second start is idempotent (one process session).
    REQUIRE(Tracing::start());

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
}
