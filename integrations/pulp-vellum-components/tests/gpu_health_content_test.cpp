// The Vellum-backed content statistics for the GPU health adapter. Each
// fixture sits on one side of one threshold of the acceptance floor, so a
// moved threshold flips exactly one case. When Pulp's own view layer is
// linked, the same images also go through it and the two verdicts must agree
// in both directions.

#include <pulp/vellum_components/gpu_health_content.hpp>

#include <catch2/catch_test_macros.hpp>

#if defined(PULP_VELLUM_COMPONENTS_HAS_PULP_VIEW)
#include <pulp/view/screenshot_compare.hpp>
#endif

#include <algorithm>
#include <cstdint>
#include <vector>

using pulp::vellum_components::analyze_png_content;

namespace {

constexpr std::uint32_t kSide = 100;
constexpr std::uint32_t kPixels = kSide * kSide;

struct Rgba {
    std::uint8_t r, g, b, a;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0) {
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(v >> shift));
}

void put_chunk(std::vector<std::uint8_t>& out, const char (&type)[5],
               const std::vector<std::uint8_t>& body) {
    put_u32(out, static_cast<std::uint32_t>(body.size()));
    std::vector<std::uint8_t> typed(type, type + 4);
    typed.insert(typed.end(), body.begin(), body.end());
    out.insert(out.end(), typed.begin(), typed.end());
    put_u32(out, crc32(typed.data(), typed.size()));
}

// An 8-bit RGBA PNG whose IDAT is a zlib stream of stored (uncompressed)
// deflate blocks, so the fixture needs no compressor.
std::vector<std::uint8_t> encode_png(const std::vector<Rgba>& pixels) {
    std::vector<std::uint8_t> raw;
    for (std::uint32_t y = 0; y < kSide; ++y) {
        raw.push_back(0); // filter: none
        for (std::uint32_t x = 0; x < kSide; ++x) {
            const auto& p = pixels[y * kSide + x];
            raw.insert(raw.end(), {p.r, p.g, p.b, p.a});
        }
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (auto v : raw) {
        a = (a + v) % 65521u;
        b = (b + a) % 65521u;
    }
    for (std::size_t pos = 0; pos < raw.size();) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n & 0xFF));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
        z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                 raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
    }
    put_u32(z, (b << 16) | a);

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    put_u32(ihdr, kSide);
    put_u32(ihdr, kSide);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    put_chunk(png, "IHDR", ihdr);
    put_chunk(png, "IDAT", z);
    put_chunk(png, "IEND", {});
    return png;
}

// A grey background with `overlay_pixels` pixels cycling over `colors`
// distinct opaque colours (so `colors` + 1 distinct colours in all), and `transparent_pixels` fully transparent ones.
std::vector<std::uint8_t> make_capture(std::uint32_t colors, std::uint32_t overlay_pixels,
                                       std::uint32_t transparent_pixels = 0) {
    constexpr std::uint8_t spread = 12;
    std::vector<Rgba> px(kPixels, Rgba{200, 200, 200, 255});
    for (std::uint32_t i = 0; i < overlay_pixels; ++i) {
        const auto k = static_cast<std::uint8_t>(i % colors);
        px[i] = Rgba{static_cast<std::uint8_t>(k * spread),
                     static_cast<std::uint8_t>(255 - k * spread),
                     static_cast<std::uint8_t>(40 + k * spread), 255};
    }
    for (std::uint32_t i = 0; i < transparent_pixels; ++i)
        px[kPixels - 1 - i] = Rgba{0, 0, 0, 0};
    return encode_png(px);
}

// Sixteen colours that differ by one blue step: enough colours, essentially no
// luminance spread.
std::vector<std::uint8_t> make_near_flat_capture() {
    std::vector<Rgba> px(kPixels);
    for (std::uint32_t i = 0; i < kPixels; ++i)
        px[i] = Rgba{200, 200, static_cast<std::uint8_t>(200 + i % 16), 255};
    return encode_png(px);
}

struct Fixture {
    const char* name;
    std::vector<std::uint8_t> png;
    bool passes;
};

std::vector<Fixture> fixtures() {
    return {
        {"blank transparent", encode_png(std::vector<Rgba>(kPixels, Rgba{0, 0, 0, 0})), false},
        {"flat opaque", encode_png(std::vector<Rgba>(kPixels, Rgba{30, 40, 50, 255})), false},
        {"16 colours in all, the floor", make_capture(15, 3000), true},
        {"15 colours in all, one short", make_capture(14, 3000), false},
        {"4% off the dominant colour", make_capture(16, 400), false},
        {"6% off the dominant colour", make_capture(16, 600), true},
        {"94% opaque", make_capture(16, 3000, 600), false},
        {"96% opaque", make_capture(16, 3000, 400), true},
        {"sixteen colours with no luminance spread", make_near_flat_capture(), false},
    };
}

} // namespace

TEST_CASE("analyze_png_content applies the acceptance floor at each threshold",
          "[pulp-vellum-components][gpu-health]") {
    for (const auto& f : fixtures()) {
        INFO(f.name);
        const auto stats = analyze_png_content(f.png);
        REQUIRE(stats.valid);
        CHECK(stats.total_pixels == kPixels);
        CHECK(stats.passes_content_floor == f.passes);
    }
}

TEST_CASE("analyze_png_content reports coverage and colour counts",
          "[pulp-vellum-components][gpu-health]") {
    const auto stats = analyze_png_content(make_capture(16, 3000, 600));
    REQUIRE(stats.valid);
    CHECK(stats.opaque_coverage > 0.93);
    CHECK(stats.opaque_coverage < 0.95);
    // 16 overlay colours, the grey background and the transparent pixels.
    CHECK(stats.unique_colors == 18);
}

TEST_CASE("analyze_png_content refuses undecodable input",
          "[pulp-vellum-components][gpu-health]") {
    for (const auto& bytes : {std::vector<std::uint8_t>{},
                              std::vector<std::uint8_t>{1, 2, 3, 4, 5},
                              std::vector<std::uint8_t>{0x89, 'P', 'N', 'G'}}) {
        const auto stats = analyze_png_content(bytes);
        CHECK_FALSE(stats.valid);
        CHECK_FALSE(stats.passes_content_floor);
    }
}

#if defined(PULP_VELLUM_COMPONENTS_HAS_PULP_VIEW)
TEST_CASE("the Vellum floor agrees with Pulp's on every fixture, in both directions",
          "[pulp-vellum-components][gpu-health]") {
    for (const auto& f : fixtures()) {
        INFO(f.name);
        const auto vellum = analyze_png_content(f.png);
        const auto pulp = pulp::view::analyze_screenshot_content(f.png);
        CHECK(vellum.valid == pulp.valid);
        CHECK(vellum.passes_content_floor == pulp.passes_content_floor());
        CHECK(vellum.total_pixels == pulp.total_pixels);
        CHECK(vellum.unique_colors == pulp.unique_colors);
        CHECK(vellum.opaque_coverage == pulp.opaque_coverage);
    }
}
#endif
