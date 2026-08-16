// test_font_woff2.cpp — WOFF2 structural rejection and decoder probing.
//
// Exercises the structural-rejection contract of
// `pulp::canvas::register_font_woff2(...)` and the
// `woff2_decoder_available()` feature probe. These tests are
// deliberately decoder-agnostic: the negative paths (null, empty,
// wrong-magic, truncated payload) must return false on every build
// regardless of whether a Brotli/woff2 implementation is linked in.
//
// Building a real .woff2 fixture would require an actual encoder,
// which Pulp does not ship. We test the structural surface only —
// that's the point of this contract: even when full decompression is
// unavailable, callers can still reliably distinguish "this isn't a
// WOFF2 file" from "this is a WOFF2 file but the build can't process
// it" through `woff2_decoder_available()`.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/text_shaper.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if PULP_HAS_SKIA
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#endif

using pulp::canvas::register_font_woff2;
using pulp::canvas::woff2_decoder_available;

namespace {

// 'wOF2' big-endian.
constexpr std::array<std::uint8_t, 4> kWoff2Magic = {0x77, 0x4F, 0x46, 0x32};

// TrueType (sfnt) magic — definitely NOT WOFF2. register_font_woff2
// must reject these even though they're a perfectly valid sfnt.
constexpr std::array<std::uint8_t, 4> kSfntMagic  = {0x00, 0x01, 0x00, 0x00};

std::vector<std::uint8_t> read_fixture(const char* path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("register_font_woff2: null pointer is rejected",
          "[font][woff2][issue-2163]") {
    REQUIRE_FALSE(register_font_woff2(nullptr, 0, ""));
    REQUIRE_FALSE(register_font_woff2(nullptr, 4096, ""));
}

TEST_CASE("register_font_woff2: empty buffer is rejected",
          "[font][woff2][issue-2163]") {
    const std::uint8_t dummy = 0;
    REQUIRE_FALSE(register_font_woff2(&dummy, 0, ""));
}

TEST_CASE("register_font_woff2: input shorter than the 4-byte magic is rejected",
          "[font][woff2][issue-2163]") {
    // Three bytes that happen to match the first three of `wOF2`.
    // Without the fourth byte we cannot trust the signature, so the
    // implementation must refuse — never read past the buffer.
    std::array<std::uint8_t, 3> too_short = {0x77, 0x4F, 0x46};
    REQUIRE_FALSE(register_font_woff2(too_short.data(), too_short.size(), ""));
}

TEST_CASE("register_font_woff2: TTF/sfnt magic is rejected (not WOFF2)",
          "[font][woff2][issue-2163]") {
    // 0x00 0x01 0x00 0x00 is a perfectly good TTF header — but this
    // entry point is specifically for WOFF2-compressed input, so the
    // wrong magic must be refused. Callers with raw TTF bytes should
    // route through register_font(...) instead.
    std::vector<std::uint8_t> bytes(kSfntMagic.begin(), kSfntMagic.end());
    bytes.resize(64, 0);
    REQUIRE_FALSE(register_font_woff2(bytes.data(), bytes.size(), ""));

    // 'OTTO' (CFF/OpenType) — same rejection.
    std::array<std::uint8_t, 4> otto = {'O', 'T', 'T', 'O'};
    std::vector<std::uint8_t> otto_buf(otto.begin(), otto.end());
    otto_buf.resize(64, 0);
    REQUIRE_FALSE(register_font_woff2(otto_buf.data(), otto_buf.size(), ""));
}

TEST_CASE("register_font_woff2: near-miss signatures are rejected",
          "[font][woff2]") {
    std::vector<std::uint8_t> lowercase = {'w', 'o', 'f', '2'};
    lowercase.resize(64, 0);
    REQUIRE_FALSE(register_font_woff2(lowercase.data(), lowercase.size(), ""));

    std::vector<std::uint8_t> leading_padding = {0, 0, 0, 0};
    leading_padding.insert(leading_padding.end(), kWoff2Magic.begin(), kWoff2Magic.end());
    leading_padding.resize(64, 0);
    REQUIRE_FALSE(register_font_woff2(leading_padding.data(), leading_padding.size(), ""));

    std::vector<std::uint8_t> partial = {0x77, 0x4F, 0x46, 0x33};
    partial.resize(64, 0);
    REQUIRE_FALSE(register_font_woff2(partial.data(), partial.size(), ""));
}

TEST_CASE("register_font_woff2: exact magic without header payload is rejected",
          "[font][woff2]") {
    std::array<std::uint8_t, 4> bytes = kWoff2Magic;
    REQUIRE_FALSE(register_font_woff2(bytes.data(), bytes.size(), ""));
    REQUIRE_FALSE(register_font_woff2(bytes.data(), bytes.size(), "Tiny WOFF2"));
}

TEST_CASE("register_font_woff2: compressed input is bounded before decoding",
          "[font][woff2][materialized-import]") {
    std::vector<std::uint8_t> bytes(16u * 1024u * 1024u + 1u, 0);
    std::copy(kWoff2Magic.begin(), kWoff2Magic.end(), bytes.begin());
    REQUIRE_FALSE(register_font_woff2(bytes.data(), bytes.size(), "Oversized"));
}

TEST_CASE("register_font_woff2: valid magic + truncated payload is rejected",
          "[font][woff2][issue-2163]") {
    // The magic is correct so the structural pre-check passes, but
    // there's no real WOFF2 header / Brotli stream behind it. On a
    // build with a real decoder linked, ComputeWOFF2FinalSize returns
    // 0 (header parse fails) and we reject. On a build without a
    // decoder, the "no decoder linked" branch returns false. Either
    // way the contract is the same: garbage with a correct magic
    // must NOT be accepted.
    std::vector<std::uint8_t> bytes(kWoff2Magic.begin(), kWoff2Magic.end());
    bytes.resize(128, 0);
    REQUIRE_FALSE(register_font_woff2(bytes.data(), bytes.size(), ""));

    // Same family-override path.
    REQUIRE_FALSE(register_font_woff2(bytes.data(), bytes.size(), "Acme Sans"));
}

TEST_CASE("woff2_decoder_available: returns a stable bool",
          "[font][woff2][issue-2163]") {
    // We don't hard-assert true or false because the answer depends on
    // build configuration (Skia + a vendored woff2/ in external/). We
    // assert the answer is stable across calls and that, if it claims
    // unavailable, register_font_woff2 still rejects the structural
    // failure modes — the universally-covered contract.
    const bool first  = woff2_decoder_available();
    const bool second = woff2_decoder_available();
    REQUIRE(first == second);

    // Structural rejection holds regardless of decoder availability.
    REQUIRE_FALSE(register_font_woff2(nullptr, 0, ""));

    std::vector<std::uint8_t> not_woff2(kSfntMagic.begin(), kSfntMagic.end());
    not_woff2.resize(32, 0);
    REQUIRE_FALSE(register_font_woff2(not_woff2.data(), not_woff2.size(), ""));
}

#if PULP_HAS_SKIA
TEST_CASE("captured WOFF2 bytes decode and register under their CSS family",
          "[font][woff2][materialized-import]") {
    REQUIRE(woff2_decoder_available());
    std::ifstream input(PULP_TEST_WOFF2_FIXTURE, std::ios::binary);
    REQUIRE(input.good());
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    REQUIRE(bytes.size() > 4);
    REQUIRE(register_font_woff2(bytes.data(), bytes.size(),
                                "Pulp Captured Inter"));
    REQUIRE(pulp::canvas::is_font_registered("Pulp Captured Inter"));
}

TEST_CASE("materialized fonts resolve exact captured faces across families",
          "[font][materialized-import][skia]") {
    struct Fixture {
        const char* path;
        const char* runtime_family;
        const char* postscript_name;
    };
    const std::array fixtures{
        Fixture{PULP_TEST_INTER_FIXTURE,
                "Inter [pulp-materialized-asset-test-inter]", "Inter-Regular"},
        Fixture{PULP_TEST_JOST_FIXTURE,
                "Jost [pulp-materialized-asset-test-jost]", "Jost-Regular"},
        Fixture{PULP_TEST_JETBRAINS_FIXTURE,
                "JetBrains Mono [pulp-materialized-asset-test-jetbrains]",
                "JetBrainsMono-Regular"},
    };

    std::array<float, fixtures.size()> widths{};
    for (std::size_t i = 0; i < fixtures.size(); ++i) {
        const auto bytes = read_fixture(fixtures[i].path);
        REQUIRE_FALSE(bytes.empty());
        REQUIRE(pulp::canvas::register_font(
            bytes.data(), bytes.size(), fixtures[i].runtime_family));

        const auto face = pulp::canvas::match_registered_typeface(
            fixtures[i].runtime_family, SkFontStyle::Normal());
        REQUIRE(face);
        SkString postscript;
        REQUIRE(face->getPostScriptName(&postscript));
        CHECK(std::string(postscript.c_str(), postscript.size()) ==
              fixtures[i].postscript_name);

        const std::string family_stack =
            std::string{"\""} + fixtures[i].runtime_family + "\", sans-serif";
        CHECK(pulp::canvas::resolved_face_identity(family_stack, 400.0f) ==
              fixtures[i].postscript_name);

        const auto shaped = pulp::canvas::global_text_shaper().prepare(
            "Ill1 Spectral", family_stack, 14.0f, 400, 0, 0.8f);
        REQUIRE(shaped.metrics_are_real());
        REQUIRE(shaped.total_width() > 0.0f);
        REQUIRE(shaped.ascent() > 0.0f);
        REQUIRE(shaped.descent() >= 0.0f);
        widths[i] = shaped.total_width();
    }

    // These faces have intentionally different glyph designs and advances.
    // Equal measurements would strongly indicate that the shaper silently
    // fell back to one platform face instead of consuming each captured font.
    CHECK(std::abs(widths[0] - widths[1]) > 0.5f);
    CHECK(std::abs(widths[0] - widths[2]) > 0.5f);
    CHECK(std::abs(widths[1] - widths[2]) > 0.5f);
}
#endif
