#pragma once

// Signed Distance Field glyph atlas — exploration prototype for #76.
//
// The goal is resolution-independent text rendering: rasterize each
// glyph once into a fixed-size SDF tile, then sample the atlas at
// runtime with a smoothstep shader to produce a crisp edge at any
// scale. Replaces the per-pixel-size bitmap atlas approach.
//
// This header defines the API. The implementation in sdf_atlas.cpp
// produces SDF tiles by rasterizing the glyph at high resolution and
// running a Euclidean distance transform (Felzenszwalb & Huttenlocher,
// 2004) over the resulting mask. A future iteration will replace this
// with FreeType's FT_RENDER_MODE_SDF for higher quality, or with a
// multi-channel SDF (Chlumsky 2015) for sharper corners at extreme
// magnifications.
//
// Status: exploration. The atlas is built and the SDF data is correct,
// but the GPU sampling shader / canvas integration is not wired in
// yet. See planning/android-sdf-glyph-atlas-76.md for the design.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulp::canvas {

// One glyph's location and metrics inside an SDF atlas.
struct SdfGlyph {
    char32_t codepoint = 0;
    int      atlas_x = 0;        // top-left in atlas pixels
    int      atlas_y = 0;
    int      width   = 0;        // tile width in atlas pixels
    int      height  = 0;        // tile height in atlas pixels
    float    bearing_x = 0.0f;   // left side bearing (pixels at base size)
    float    bearing_y = 0.0f;   // top bearing (positive up)
    float    advance   = 0.0f;   // glyph advance (pixels at base size)
};

// Single-channel SDF atlas. Each texel stores the signed distance
// from the texel center to the nearest glyph edge, mapped to
// [0, 255] where 128 = on the edge.
class SdfAtlas {
public:
    SdfAtlas();
    ~SdfAtlas();

    SdfAtlas(const SdfAtlas&) = delete;
    SdfAtlas& operator=(const SdfAtlas&) = delete;
    SdfAtlas(SdfAtlas&&) noexcept;
    SdfAtlas& operator=(SdfAtlas&&) noexcept;

    // Build an atlas containing every glyph in `chars`. The atlas is
    // rendered at `base_size` pixels per em with `padding` texels of
    // empty space around each glyph (the SDF spread radius).
    //
    // `font_family` is resolved via the same path as canvas text
    // rendering, so any font visible to SkFontMgr will work. Returns
    // false if the font could not be loaded or the resulting atlas
    // would exceed `max_atlas_size` on either axis.
    bool build(const std::string& font_family,
               const std::vector<char32_t>& chars,
               int base_size = 48,
               int padding   = 8,
               int max_atlas_size = 2048);

    // Look up an SDF glyph by codepoint. Returns nullptr if the glyph
    // was not in the build set.
    const SdfGlyph* glyph(char32_t codepoint) const;

    // Atlas image data. One byte per texel, row-major, no padding.
    // `width()` * `height()` bytes long. The buffer is owned by the
    // atlas and is valid for its lifetime.
    const std::uint8_t* pixels() const;
    int width()  const;
    int height() const;

    // Number of glyphs successfully packed into the atlas.
    std::size_t glyph_count() const;

    // Base font size the atlas was built at. Glyph metrics are
    // expressed in pixels at this size; downstream code that draws at
    // a different size scales the metrics linearly and the SDF shader
    // smooths the edges.
    int base_size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::canvas
