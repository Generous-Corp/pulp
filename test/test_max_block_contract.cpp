// Unit coverage for the one max-block-overrun contract shared by every format
// adapter (CLAP/VST3/AU-v3/AAX). The adapters' render callbacks themselves are
// host/SDK-gated, but the clamp decision they all now route through is pure and
// testable here — this is the invariant that keeps the four adapters from
// diverging again (AU-v3 used to reject an overrun; AAX used to re-prepare on
// the render thread).

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/max_block_contract.hpp>

using pulp::format::clamp_block_to_prepared_max;

TEST_CASE("max-block contract: block within the prepared max is unchanged",
          "[format][max-block]") {
    CHECK(clamp_block_to_prepared_max(256, 512) == 256);
    CHECK(clamp_block_to_prepared_max(512, 512) == 512);
    CHECK(clamp_block_to_prepared_max(1, 512) == 1);
    CHECK(clamp_block_to_prepared_max(0, 512) == 0);
}

TEST_CASE("max-block contract: an overrun clamps to the prepared max",
          "[format][max-block]") {
    CHECK(clamp_block_to_prepared_max(513, 512) == 512);
    CHECK(clamp_block_to_prepared_max(4096, 512) == 512);
    // The clamped tail [prepared_max, requested) is what each adapter then
    // zero-fills; the count is requested - clamped.
    CHECK(clamp_block_to_prepared_max(600, 512) == 512);
    CHECK(600 - clamp_block_to_prepared_max(600, 512) == 88);
}

TEST_CASE("max-block contract: an unset max (<= 0) is treated as unbounded",
          "[format][max-block]") {
    CHECK(clamp_block_to_prepared_max(1024, 0) == 1024);
    CHECK(clamp_block_to_prepared_max(1024, -1) == 1024);
    CHECK(clamp_block_to_prepared_max(0, 0) == 0);
}

TEST_CASE("max-block contract: constexpr-evaluable (compile-time invariant)",
          "[format][max-block]") {
    static_assert(clamp_block_to_prepared_max(256, 512) == 256);
    static_assert(clamp_block_to_prepared_max(1024, 512) == 512);
    static_assert(clamp_block_to_prepared_max(128, 0) == 128);
    SUCCEED();
}
