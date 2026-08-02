#include "../tools/cli/overflow_selector.hpp"

#include <catch2/catch_test_macros.hpp>

using pulp::cli::validate_overflow_selector;

TEST_CASE("overflow selector accepts one safe label or a safe label array", "[cli][overflow]") {
    REQUIRE(validate_overflow_selector("\"macos-15\"").valid);
    REQUIRE(validate_overflow_selector("[\"self-hosted\",\"macOS\",\"ARM64\",\"pulp-gate-fast\"]")
                .valid);
}

TEST_CASE("overflow selector recognizes every encoded off switch", "[cli][overflow]") {
    for (const auto* selector : {"local-only", "  local-only  ", "\"local-only\"",
                                 "\"local\\u002donly\"", "[\"local-only\"]"}) {
        const auto result = validate_overflow_selector(selector);
        REQUIRE_FALSE(result.valid);
        REQUIRE(result.is_off_switch);
    }
}

TEST_CASE("overflow selector rejects malformed and unsafe labels", "[cli][overflow]") {
    for (const auto* selector :
         {"local-only", "null", "{}", "[]", "[\"macos-15\",2]", "\"macos-15\" trailing",
          "\"macos-15\"}garbage", "\"macos\\q15\"", "\"macos-15\"]garbage", "\"macos-15",
          "[\"macos-15\"", "[\"macos-15\",]", "[\"macos-15\"],garbage", "\"macos 15\"",
          "\"macos\\u00a015\"", "\"macos-\\u202e15\""}) {
        INFO(selector);
        REQUIRE_FALSE(validate_overflow_selector(selector).valid);
    }
}
