// Native coverage for pulp/runtime/exceptions.hpp — the portable try/catch
// layer that lets defensive blocks compile both natively and under the wasi-sdk
// `-fno-exceptions` toolchain used by the WebAssembly plugin targets.
//
// This test runs with exceptions ENABLED (the native build), so it pins the
// "exceptions on" expansion: PULP_TRY runs the guarded block, PULP_CATCH_ALL
// catches a real throw, and a non-throwing block falls through past the handler.
// The "exceptions off" expansion (guarded block runs unconditionally, handler
// dead-stripped) is exercised by the actual wasi-sdk WebCLAP/WAM builds — there
// is no -fno-exceptions native build to assert it here.

#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/exceptions.hpp>

#include <stdexcept>
#include <string>

namespace {

// A defensive helper shaped exactly like the real call sites
// (plugin_state_io.cpp, the wasm preset path): try the work, swallow any
// failure, return a safe fallback.
std::string classify(int x) {
    PULP_TRY {
        if (x < 0) throw std::runtime_error("negative");
        return "ok";
    } PULP_CATCH_ALL {
        return "fallback";
    }
    return "unreached";
}

} // namespace

TEST_CASE("PULP_TRY runs the guarded block when nothing throws", "[runtime][exceptions]") {
    REQUIRE(classify(7) == "ok");
}

TEST_CASE("PULP_CATCH_ALL catches a thrown exception (exceptions enabled)",
          "[runtime][exceptions]") {
    // This build has exceptions on, so the throw is caught and the fallback wins.
    REQUIRE(classify(-1) == "fallback");
}

TEST_CASE("A PULP_TRY block with no throw never falls into the handler",
          "[runtime][exceptions]") {
    int handler_runs = 0;
    int value = 0;
    PULP_TRY {
        value = 42;
    } PULP_CATCH_ALL {
        ++handler_runs;  // unreachable when nothing throws
    }
    REQUIRE(value == 42);
    REQUIRE(handler_runs == 0);
}

TEST_CASE("The macros compile around early returns and nested scopes",
          "[runtime][exceptions]") {
    auto first_positive = [](int a, int b) -> int {
        PULP_TRY {
            if (a > 0) return a;
            if (b > 0) return b;
            throw std::logic_error("none positive");
        } PULP_CATCH_ALL {
            return -1;
        }
        return -2;
    };
    REQUIRE(first_positive(3, 9) == 3);
    REQUIRE(first_positive(-1, 9) == 9);
    REQUIRE(first_positive(-1, -1) == -1);
}
