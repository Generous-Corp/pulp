// Multi-channel SDF atlas — Phase 2 scaffold.
//
// This file intentionally implements only the API surface. Real MSDF
// generation ships with the msdfgen (BSD-2-Clause / MIT) integration
// in a follow-up commit that wires FetchContent, adds the include path,
// and replaces `generate_msdf_tile()` with a call into msdfgen.
//
// The scaffold is useful today because:
//   * it makes the public header compilable and testable;
//   * the glyph-packing logic mirrors SdfAtlas so the MSDF variant will
//     slot in without rewriting the atlas layout code; and
//   * the stub `generate_msdf_tile()` writes a legible gradient so
//     visual tests can verify the sampler shader is wired up before
//     real msdfgen output is available.

#include <pulp/canvas/msdf_atlas.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace pulp::canvas {

namespace {

// Until msdfgen is vendored, write a radial-gradient RGB tile. All three
// channels store the same distance, so `median(r, g, b)` in the shader
// degenerates to the single-channel result — visually identical to
// `SdfAtlas`. Once msdfgen is wired in, this helper is replaced with
// its per-channel output.
void fill_placeholder_tile(std::uint8_t* rgb,
                           int tile_w, int tile_h,
                           int atlas_w, int x0, int y0) {
    const float cx = tile_w * 0.5f;
    const float cy = tile_h * 0.5f;
    const float r  = std::min(cx, cy) * 0.7f;
    const float spread = std::max(cx, cy) - r;
    for (int y = 0; y < tile_h; ++y) {
        for (int x = 0; x < tile_w; ++x) {
            const float dx = x + 0.5f - cx;
            const float dy = y + 0.5f - cy;
            const float d  = std::sqrt(dx * dx + dy * dy);
            // Map signed distance into [0, 1] with edge at 0.5.
            const float sd = 0.5f - (d - r) / (2.0f * spread);
            const auto v = static_cast<std::uint8_t>(
                std::clamp(sd * 255.0f, 0.0f, 255.0f));
            const int offset = ((y0 + y) * atlas_w + (x0 + x)) * 3;
            rgb[offset + 0] = v;
            rgb[offset + 1] = v;
            rgb[offset + 2] = v;
        }
    }
}

} // namespace

struct MsdfAtlas::Impl {
    std::vector<std::uint8_t> pixels;  // RGB8, row-major
    std::unordered_map<char32_t, MsdfGlyph> glyphs;
    int width = 0;
    int height = 0;
    int base_size = 0;
};

MsdfAtlas::MsdfAtlas() = default;
MsdfAtlas::~MsdfAtlas() = default;
MsdfAtlas::MsdfAtlas(MsdfAtlas&&) noexcept = default;
MsdfAtlas& MsdfAtlas::operator=(MsdfAtlas&&) noexcept = default;

bool MsdfAtlas::build(const std::string& /*font_family*/,
                      const std::vector<char32_t>& chars,
                      int base_size,
                      int padding,
                      int max_atlas_size) {
    impl_ = std::make_unique<Impl>();
    impl_->base_size = base_size;

    const int tile_w = base_size + 2 * padding;
    const int tile_h = base_size + 2 * padding;
    const int count  = static_cast<int>(chars.size());
    if (count == 0) return true;

    const int cols_needed = std::max(1,
        static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count)))));
    const int rows_needed = (count + cols_needed - 1) / cols_needed;
    impl_->width  = cols_needed * tile_w;
    impl_->height = rows_needed * tile_h;
    if (impl_->width > max_atlas_size || impl_->height > max_atlas_size) {
        impl_ = std::make_unique<Impl>();
        return false;
    }

    impl_->pixels.assign(
        static_cast<std::size_t>(impl_->width * impl_->height * 3), 0);

    for (int i = 0; i < count; ++i) {
        const int col = i % cols_needed;
        const int row = i / cols_needed;
        const int x0  = col * tile_w;
        const int y0  = row * tile_h;
        fill_placeholder_tile(impl_->pixels.data(),
                              tile_w, tile_h, impl_->width, x0, y0);

        MsdfGlyph g;
        g.codepoint = chars[i];
        g.atlas_x = x0 + padding;
        g.atlas_y = y0 + padding;
        g.width   = base_size;
        g.height  = base_size;
        g.bearing_x = 0.0f;
        g.bearing_y = static_cast<float>(base_size);
        g.advance   = static_cast<float>(base_size);
        impl_->glyphs[chars[i]] = g;
    }
    return true;
}

const MsdfGlyph* MsdfAtlas::glyph(char32_t codepoint) const {
    if (!impl_) return nullptr;
    auto it = impl_->glyphs.find(codepoint);
    return it == impl_->glyphs.end() ? nullptr : &it->second;
}

const std::uint8_t* MsdfAtlas::pixels() const {
    return impl_ ? impl_->pixels.data() : nullptr;
}
int MsdfAtlas::width()  const { return impl_ ? impl_->width  : 0; }
int MsdfAtlas::height() const { return impl_ ? impl_->height : 0; }
std::size_t MsdfAtlas::glyph_count() const {
    return impl_ ? impl_->glyphs.size() : 0;
}
int MsdfAtlas::base_size() const { return impl_ ? impl_->base_size : 0; }

} // namespace pulp::canvas
