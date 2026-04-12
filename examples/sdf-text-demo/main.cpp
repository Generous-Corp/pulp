// SDF vs MSDF text quality demo.
//
// Builds both atlases for a short alphanumeric range and writes the
// pixel buffers to disk so they can be inspected side-by-side:
//
//   /tmp/pulp-sdf-atlas.pgm     — single-channel (grayscale)
//   /tmp/pulp-msdf-atlas.ppm    — three-channel (RGB)
//
// A follow-up iteration connects these atlases to the runtime sampler
// shaders (core/canvas/shaders/{sdf,msdf}_text.sksl) and renders a
// side-by-side preview window via pulp::view. The atlases are the
// hard part; the render side is mechanical.

#include <pulp/canvas/msdf_atlas.hpp>
#include <pulp/canvas/sdf_atlas.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool write_pgm(const std::string& path,
               const std::uint8_t* pixels, int w, int h) {
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    std::fprintf(fp, "P5\n%d %d\n255\n", w, h);
    std::fwrite(pixels, 1, static_cast<std::size_t>(w * h), fp);
    std::fclose(fp);
    return true;
}

bool write_ppm(const std::string& path,
               const std::uint8_t* rgb, int w, int h) {
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    std::fprintf(fp, "P6\n%d %d\n255\n", w, h);
    std::fwrite(rgb, 1, static_cast<std::size_t>(w * h * 3), fp);
    std::fclose(fp);
    return true;
}

}  // namespace

int main() {
    std::vector<char32_t> chars;
    for (char32_t c = U'0'; c <= U'9'; ++c) chars.push_back(c);
    for (char32_t c = U'A'; c <= U'Z'; ++c) chars.push_back(c);
    for (char32_t c = U'a'; c <= U'z'; ++c) chars.push_back(c);

    pulp::canvas::SdfAtlas sdf;
    if (!sdf.build("", chars, 48, 6, 2048)) {
        std::fprintf(stderr, "SdfAtlas::build failed\n");
        return 1;
    }
    write_pgm("/tmp/pulp-sdf-atlas.pgm",
              sdf.pixels(), sdf.width(), sdf.height());

    pulp::canvas::MsdfAtlas msdf;
    if (!msdf.build("", chars, 48, 6, 2048)) {
        std::fprintf(stderr, "MsdfAtlas::build failed\n");
        return 1;
    }
    write_ppm("/tmp/pulp-msdf-atlas.ppm",
              msdf.pixels(), msdf.width(), msdf.height());

    std::printf("Wrote SDF atlas:  %dx%d  → /tmp/pulp-sdf-atlas.pgm\n",
                sdf.width(), sdf.height());
    std::printf("Wrote MSDF atlas: %dx%d  → /tmp/pulp-msdf-atlas.ppm\n",
                msdf.width(), msdf.height());
    std::printf("Glyphs: %zu (SDF) / %zu (MSDF)\n",
                sdf.glyph_count(), msdf.glyph_count());
    return 0;
}
