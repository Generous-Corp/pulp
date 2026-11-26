// test_audio_png_encoder.cpp -- the PNG writer behind `pulp audio` images.
//
// The checks parse the bytes back by hand (signature, chunk CRCs, IHDR, zlib
// inflate, scanline filter bytes) instead of trusting a decoder, so a wrong
// CRC, length or filter byte fails on every platform.

#include <catch2/catch_test_macros.hpp>

#include "audio_png_encoder.hpp"

#include <pulp/runtime/zip.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
           std::uint32_t(p[3]);
}

std::uint32_t reference_crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t c = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xffffffffu;
}

struct Parsed {
    bool valid = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> idat;
    std::string order; ///< Chunk types in file order, e.g. "IHDR,IDAT,IEND".
};

Parsed parse(const std::vector<std::uint8_t>& png) {
    Parsed out;
    static const std::array<std::uint8_t, 8> sig{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (png.size() < 8 || std::memcmp(png.data(), sig.data(), 8) != 0)
        return out;
    std::size_t at = 8;
    while (at + 12 <= png.size()) {
        const std::uint32_t length = be32(&png[at]);
        if (at + 12 + length > png.size())
            return out;
        const auto* type = &png[at + 4];
        if (be32(&png[at + 8 + length]) != reference_crc32(type, 4 + std::size_t(length)))
            return out;
        const std::string name(reinterpret_cast<const char*>(type), 4);
        out.order += (out.order.empty() ? "" : ",") + name;
        if (name == "IHDR" && length == 13) {
            out.width = be32(&png[at + 8]);
            out.height = be32(&png[at + 12]);
            if (png[at + 16] != 8 || png[at + 17] != 6 || png[at + 18] != 0 || png[at + 19] != 0 ||
                png[at + 20] != 0)
                return out;
        } else if (name == "IDAT") {
            out.idat.insert(out.idat.end(), &png[at + 8], &png[at + 8 + length]);
        }
        at += 12 + length;
    }
    out.valid = at == png.size();
    return out;
}

std::vector<std::uint8_t> gradient(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> rgba(std::size_t(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            const auto i = (std::size_t(y) * w + x) * 4;
            rgba[i + 0] = std::uint8_t(x * 7 + y);
            rgba[i + 1] = std::uint8_t(y * 13);
            rgba[i + 2] = std::uint8_t(x ^ y);
            rgba[i + 3] = std::uint8_t(255 - x);
        }
    return rgba;
}

} // namespace

TEST_CASE("encode_png_rgba8 writes a PNG that parses back to the input pixels",
          "[audio][png][encoder]") {
    const std::uint32_t w = 37, h = 11; // odd sizes catch row-stride mistakes
    const auto rgba = gradient(w, h);
    std::string error;
    const auto png = pulp_cli::encode_png_rgba8(w, h, rgba, &error);
    REQUIRE_FALSE(png.empty());
    CHECK(error.empty());

    const auto parsed = parse(png);
    REQUIRE(parsed.valid);
    CHECK(parsed.order == "IHDR,IDAT,IEND");
    CHECK(parsed.width == w);
    CHECK(parsed.height == h);

    const auto raw = pulp::runtime::gzip_decompress(parsed.idat.data(), parsed.idat.size());
    REQUIRE(raw.has_value());
    REQUIRE(raw->size() == (std::size_t(w) * 4 + 1) * h);
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto* line = raw->data() + y * (std::size_t(w) * 4 + 1);
        CHECK(line[0] == 0); // filter type none
        CHECK(std::memcmp(line + 1, rgba.data() + std::size_t(y) * w * 4, std::size_t(w) * 4) == 0);
    }
}

TEST_CASE("encode_png_rgba8 refuses a buffer that does not match its size",
          "[audio][png][encoder]") {
    std::string error;
    CHECK(
        pulp_cli::encode_png_rgba8(4, 4, std::vector<std::uint8_t>(4 * 4 * 4 - 1), &error).empty());
    CHECK(error.find("does not match") != std::string::npos);

    error.clear();
    CHECK(pulp_cli::encode_png_rgba8(0, 4, {}, &error).empty());
    CHECK(error.find("non-zero") != std::string::npos);

    // No error sink is also accepted.
    CHECK(pulp_cli::encode_png_rgba8(4, 0, {}, nullptr).empty());
}

TEST_CASE("the test's PNG parser rejects a corrupted chunk", "[audio][png][encoder]") {
    auto png = pulp_cli::encode_png_rgba8(8, 8, gradient(8, 8), nullptr);
    REQUIRE(parse(png).valid);
    png[png.size() / 2] ^= 0x40; // a flipped payload bit breaks a chunk CRC
    CHECK_FALSE(parse(png).valid);
}
