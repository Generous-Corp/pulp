// test_font_woff2.cpp — Pulp #2163, font v2 Slice 3.5.
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
// that's the point of Slice 3.5: even when full decompression is
// unavailable, callers can still reliably distinguish "this isn't a
// WOFF2 file" from "this is a WOFF2 file but the build can't process
// it" through `woff2_decoder_available()`.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/bundled_fonts.hpp>

#include <array>
#include <cstdint>
#include <vector>

using pulp::canvas::register_font_woff2;
using pulp::canvas::woff2_decoder_available;

namespace {

// 'wOF2' big-endian.
constexpr std::array<std::uint8_t, 4> kWoff2Magic = {0x77, 0x4F, 0x46, 0x32};

// TrueType (sfnt) magic — definitely NOT WOFF2. register_font_woff2
// must reject these even though they're a perfectly valid sfnt.
constexpr std::array<std::uint8_t, 4> kSfntMagic  = {0x00, 0x01, 0x00, 0x00};

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
