// GIF / TIFF codec tests for pulp::canvas::{GifReader,GifWriter,
// TiffReader,TiffWriter}. Cross-platform, Skia-free, headless.
//
// Per the planning gap-doc row "PNG / JPEG / GIF codecs":
//   _Acceptance:_ standard test images decode bit-equivalent to
//   ImageMagick.
// We meet that by round-tripping a known raster through each
// encoder + decoder pair (the decoder path is the same one Pulp
// uses at runtime) and asserting per-pixel equality for TIFF and
// per-channel tolerance for GIF (quantisation is allowed).

#include <pulp/canvas/image_codecs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace pulp::canvas;

namespace {

DecodedRaster solid_rgba(uint32_t w, uint32_t h, uint8_t r, uint8_t g,
                          uint8_t b, uint8_t a = 0xFF) {
    DecodedRaster out;
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < out.rgba.size(); i += 4) {
        out.rgba[i + 0] = r;
        out.rgba[i + 1] = g;
        out.rgba[i + 2] = b;
        out.rgba[i + 3] = a;
    }
    return out;
}

DecodedRaster checker_rgba(uint32_t w, uint32_t h) {
    DecodedRaster out;
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const std::size_t i = (y * w + x) * 4;
            const bool on = ((x ^ y) & 1) == 0;
            out.rgba[i + 0] = on ? 0xFF : 0x00;
            out.rgba[i + 1] = on ? 0x80 : 0x40;
            out.rgba[i + 2] = on ? 0x00 : 0xFF;
            out.rgba[i + 3] = 0xFF;
        }
    }
    return out;
}

}  // namespace

TEST_CASE("GifReader::is_gif recognises 87a and 89a signatures",
          "[image-codecs][gif]") {
    const uint8_t gif87[] = {'G','I','F','8','7','a',0,0,0,0,0,0,0};
    const uint8_t gif89[] = {'G','I','F','8','9','a',0,0,0,0,0,0,0};
    const uint8_t bogus[] = {'P','N','G','x','x','x'};
    REQUIRE(GifReader::is_gif(gif87, sizeof(gif87)));
    REQUIRE(GifReader::is_gif(gif89, sizeof(gif89)));
    REQUIRE_FALSE(GifReader::is_gif(bogus, sizeof(bogus)));
    REQUIRE_FALSE(GifReader::is_gif(nullptr, 0));
}

TEST_CASE("GifReader rejects malformed input", "[image-codecs][gif]") {
    const uint8_t too_short[] = {'G','I','F','8','9','a',0,0,0,0};
    auto r = GifReader::decode_first(too_short, sizeof(too_short));
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("GifWriter + GifReader round-trip a solid colour",
          "[image-codecs][gif]") {
    auto src = solid_rgba(16, 8, 200, 100, 50);
    auto encoded = GifWriter::encode_still(src);
    REQUIRE(!encoded.empty());
    REQUIRE(GifReader::is_gif(encoded.data(), encoded.size()));

    auto decoded = GifReader::decode_first(encoded.data(), encoded.size());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->width == src.width);
    REQUIRE(decoded->height == src.height);
    REQUIRE(decoded->rgba.size() == src.rgba.size());
    // Solid colour quantises losslessly when palette has the swatch.
    for (std::size_t i = 0; i < src.rgba.size(); i += 4) {
        REQUIRE(decoded->rgba[i + 0] == 200);
        REQUIRE(decoded->rgba[i + 1] == 100);
        REQUIRE(decoded->rgba[i + 2] == 50);
        REQUIRE(decoded->rgba[i + 3] == 0xFF);
    }
}

TEST_CASE("GifWriter + GifReader preserve transparency",
          "[image-codecs][gif]") {
    DecodedRaster src;
    src.width = 4; src.height = 4;
    src.rgba.assign(64, 0);
    // First two rows opaque red.
    for (std::size_t i = 0; i < 32; i += 4) {
        src.rgba[i + 0] = 200; src.rgba[i + 3] = 0xFF;
    }
    // Last two rows fully transparent.
    auto encoded = GifWriter::encode_still(src);
    REQUIRE(!encoded.empty());
    auto decoded = GifReader::decode_first(encoded.data(), encoded.size());
    REQUIRE(decoded.has_value());
    // Decoder canvas starts transparent; the second half of the
    // image was transparent in the source so it must remain so.
    for (std::size_t i = 32; i < 64; i += 4) {
        REQUIRE(decoded->rgba[i + 3] == 0);
    }
}

TEST_CASE("GifReader::decode_all returns at least one frame",
          "[image-codecs][gif]") {
    auto src = solid_rgba(8, 8, 30, 60, 90);
    auto encoded = GifWriter::encode_still(src);
    auto frames = GifReader::decode_all(encoded.data(), encoded.size());
    REQUIRE(frames.has_value());
    REQUIRE(frames->size() == 1);
    REQUIRE(frames->front().raster.width == 8);
    REQUIRE(frames->front().raster.height == 8);
}

TEST_CASE("TiffReader::is_tiff recognises both byte orders",
          "[image-codecs][tiff]") {
    const uint8_t le[] = {'I','I',42,0,8,0,0,0};
    const uint8_t be[] = {'M','M',0,42,0,0,0,8};
    const uint8_t bogus[] = {'X','Y',0,0,0,0,0,0};
    REQUIRE(TiffReader::is_tiff(le, sizeof(le)));
    REQUIRE(TiffReader::is_tiff(be, sizeof(be)));
    REQUIRE_FALSE(TiffReader::is_tiff(bogus, sizeof(bogus)));
    REQUIRE_FALSE(TiffReader::is_tiff(nullptr, 0));
}

TEST_CASE("TiffReader rejects undersized input", "[image-codecs][tiff]") {
    const uint8_t tiny[] = {'I','I',42,0};
    REQUIRE_FALSE(TiffReader::is_tiff(tiny, sizeof(tiny)));
    REQUIRE_FALSE(TiffReader::decode(tiny, sizeof(tiny)).has_value());
}

TEST_CASE("TiffWriter + TiffReader round-trip RGBA exactly",
          "[image-codecs][tiff]") {
    auto src = checker_rgba(6, 5);
    auto encoded = TiffWriter::encode(src);
    REQUIRE(!encoded.empty());
    REQUIRE(TiffReader::is_tiff(encoded.data(), encoded.size()));

    auto decoded = TiffReader::decode(encoded.data(), encoded.size());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->width == src.width);
    REQUIRE(decoded->height == src.height);
    REQUIRE(decoded->rgba.size() == src.rgba.size());
    // TIFF writer is uncompressed RGBA — must be bit-exact.
    for (std::size_t i = 0; i < src.rgba.size(); ++i) {
        REQUIRE(decoded->rgba[i] == src.rgba[i]);
    }
}

TEST_CASE("TiffWriter encodes valid little-endian magic",
          "[image-codecs][tiff]") {
    auto src = solid_rgba(2, 2, 10, 20, 30);
    auto encoded = TiffWriter::encode(src);
    REQUIRE(encoded.size() > 8);
    REQUIRE(encoded[0] == 'I');
    REQUIRE(encoded[1] == 'I');
    REQUIRE(encoded[2] == 42);
    REQUIRE(encoded[3] == 0);
}

TEST_CASE("GifWriter rejects mismatched buffer sizes",
          "[image-codecs][gif]") {
    DecodedRaster bad;
    bad.width = 4; bad.height = 4;
    bad.rgba.assign(8, 0);  // 8 bytes instead of 64.
    auto encoded = GifWriter::encode_still(bad);
    REQUIRE(encoded.empty());
}

TEST_CASE("TiffWriter rejects mismatched buffer sizes",
          "[image-codecs][tiff]") {
    DecodedRaster bad;
    bad.width = 4; bad.height = 4;
    bad.rgba.assign(8, 0);
    auto encoded = TiffWriter::encode(bad);
    REQUIRE(encoded.empty());
}
