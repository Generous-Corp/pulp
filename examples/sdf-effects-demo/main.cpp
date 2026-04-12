// SDF effects showcase.
//
// For each design-token preset (subtle shadow, outline, glow, pressed
// bevel) renders the same "PULP" text via the software reference
// renderer and writes one PGM file per effect. A later iteration swaps
// the software path for the SkSL effects shader once SkiaCanvas grows
// a draw_sdf_text() call-site; the host-side API surface stays
// identical because `SdfEffectParams` already mirrors the shader
// uniforms.

#include <pulp/canvas/sdf_atlas.hpp>
#include <pulp/canvas/sdf_effects.hpp>
#include <pulp/canvas/sdf_software_renderer.hpp>
#include <pulp/canvas/sdf_text.hpp>

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
}  // namespace

int main() {
    pulp::canvas::SdfAtlas atlas;
    const std::vector<char32_t> chars = {U'P', U'U', U'L'};
    if (!atlas.build("", chars, 48, 6, 1024)) {
        std::fprintf(stderr, "SdfAtlas::build failed\n");
        return 1;
    }

    const auto quads = pulp::canvas::build_text_quads(
        atlas, std::u32string(U"PULP"),
        /*x*/ 16.0f, /*y*/ 64.0f, /*render_size*/ 48.0f);

    // Each preset writes one PGM. The software renderer does not yet
    // interpret the per-effect shader, but emitting the files per
    // preset exercises the end-to-end path and makes the demo a
    // placeholder the SkSL path can slot into.
    struct Variant {
        const char* name;
        pulp::canvas::SdfEffectParams params;
    };
    const std::vector<Variant> variants = {
        {"plain",          {}},
        {"subtle_shadow",  pulp::canvas::preset_subtle_shadow()},
        {"outline",        pulp::canvas::preset_outline(2.0f)},
        {"glow",           pulp::canvas::preset_glow()},
        {"pressed_bevel",  pulp::canvas::preset_pressed_bevel()},
    };

    constexpr int W = 256;
    constexpr int H = 96;
    for (const auto& v : variants) {
        std::vector<std::uint8_t> buf(W * H, 0);
        pulp::canvas::render_sdf_text_software(atlas, quads, buf.data(), W, H);
        const auto path = std::string("/tmp/pulp-sdf-effects-") + v.name + ".pgm";
        write_pgm(path, buf.data(), W, H);
        std::printf("%-16s → %s (active=0x%02x)\n",
                    v.name, path.c_str(),
                    static_cast<unsigned>(v.params.active));
    }
    return 0;
}
