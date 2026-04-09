// SDF glyph atlas — exploration implementation for #76.
//
// Algorithm:
//   1. For each requested codepoint, rasterize a binary mask at
//      base_size + 2*padding using a glyph rasterizer (currently a
//      stub that produces a circle for testing — Skia integration is
//      the next step).
//   2. Run a 2-pass Euclidean distance transform (Felzenszwalb &
//      Huttenlocher 2004) over the mask to produce signed distance
//      values in pixels.
//   3. Map the distance to [0, 255] with 128 = edge, clamped to a
//      range of [-padding, +padding] pixels.
//   4. Pack tiles into a single atlas image using a simple shelf
//      packer.
//
// The placeholder rasterizer is intentionally minimal: it produces a
// filled circle whose radius scales with the codepoint, so tests can
// verify that the SDF distance values are correct without depending
// on a specific font being installed. Real glyph rendering follows
// in the next iteration when Skia is wired in.

#include <pulp/canvas/sdf_atlas.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace pulp::canvas {

// ── Distance transform ──────────────────────────────────────────────────
//
// Felzenszwalb-Huttenlocher 1D pass: takes a row of "infinity for
// outside, 0 for inside" values and produces, for each cell, the
// squared distance to the nearest 0. Two passes (rows then columns)
// over the squared values produces a 2D Euclidean distance transform.

namespace {

constexpr float kInf = 1e20f;

void edt_1d(const float* f, float* d, int n) {
    std::vector<int> v(static_cast<std::size_t>(n));
    std::vector<float> z(static_cast<std::size_t>(n + 1));
    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] =  kInf;
    for (int q = 1; q < n; ++q) {
        float s = ((f[q] + static_cast<float>(q * q))
                  - (f[v[k]] + static_cast<float>(v[k] * v[k])))
                  / static_cast<float>(2 * q - 2 * v[k]);
        while (s <= z[k]) {
            --k;
            s = ((f[q] + static_cast<float>(q * q))
                - (f[v[k]] + static_cast<float>(v[k] * v[k])))
                / static_cast<float>(2 * q - 2 * v[k]);
        }
        ++k;
        v[k]   = q;
        z[k]   = s;
        z[k+1] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k+1] < static_cast<float>(q)) ++k;
        float dx = static_cast<float>(q - v[k]);
        d[q] = dx * dx + f[v[k]];
    }
}

// 2D EDT on a binary mask: 1 = inside, 0 = outside. Produces a
// pixel-wise floating-point distance image, positive outside the
// shape, negative inside. Same width × height as input.
std::vector<float> distance_transform(const std::uint8_t* mask, int w, int h) {
    std::vector<float> outside(static_cast<std::size_t>(w * h));
    std::vector<float> inside(static_cast<std::size_t>(w * h));
    for (int i = 0; i < w * h; ++i) {
        if (mask[i] != 0) {
            outside[static_cast<std::size_t>(i)] = 0.0f;
            inside[static_cast<std::size_t>(i)]  = kInf;
        } else {
            outside[static_cast<std::size_t>(i)] = kInf;
            inside[static_cast<std::size_t>(i)]  = 0.0f;
        }
    }
    auto pass2d = [&](std::vector<float>& buf) {
        std::vector<float> col(static_cast<std::size_t>(std::max(w, h)));
        std::vector<float> col_out(static_cast<std::size_t>(std::max(w, h)));
        // Rows
        for (int y = 0; y < h; ++y) {
            edt_1d(buf.data() + y * w, col_out.data(), w);
            std::copy_n(col_out.begin(), w, buf.begin() + y * w);
        }
        // Columns
        for (int x = 0; x < w; ++x) {
            for (int y = 0; y < h; ++y) col[static_cast<std::size_t>(y)] = buf[static_cast<std::size_t>(y * w + x)];
            edt_1d(col.data(), col_out.data(), h);
            for (int y = 0; y < h; ++y) buf[static_cast<std::size_t>(y * w + x)] = col_out[static_cast<std::size_t>(y)];
        }
    };
    pass2d(outside);
    pass2d(inside);

    std::vector<float> result(static_cast<std::size_t>(w * h));
    for (int i = 0; i < w * h; ++i) {
        float d_out = std::sqrt(outside[static_cast<std::size_t>(i)]);
        float d_in  = std::sqrt(inside[static_cast<std::size_t>(i)]);
        result[static_cast<std::size_t>(i)] = d_out - d_in;
    }
    return result;
}

// Placeholder glyph rasterizer: draws a filled circle whose radius is
// derived from the codepoint, so the test suite can validate the
// distance transform without depending on a font.
// Returns a binary mask of size w × h.
std::vector<std::uint8_t> rasterize_placeholder(char32_t codepoint, int w, int h, int padding) {
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(w * h), 0);
    float cx = w * 0.5f;
    float cy = h * 0.5f;
    float radius = static_cast<float>((w - 2 * padding)) * 0.4f;
    // Vary radius slightly so different codepoints give different shapes
    radius *= 0.7f + 0.3f * std::sin(static_cast<float>(codepoint));
    if (radius < 1.0f) radius = 1.0f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float dx = static_cast<float>(x) - cx;
            float dy = static_cast<float>(y) - cy;
            if (std::sqrt(dx * dx + dy * dy) <= radius) {
                mask[static_cast<std::size_t>(y * w + x)] = 1;
            }
        }
    }
    return mask;
}

} // anonymous namespace

// ── SdfAtlas::Impl ───────────────────────────────────────────────────────

struct SdfAtlas::Impl {
    int base_size = 0;
    int padding   = 0;
    int width     = 0;
    int height    = 0;
    std::vector<std::uint8_t> pixels;  // grayscale, 1 byte per texel
    std::unordered_map<char32_t, SdfGlyph> glyphs;
};

SdfAtlas::SdfAtlas() = default;
SdfAtlas::~SdfAtlas() = default;
SdfAtlas::SdfAtlas(SdfAtlas&&) noexcept = default;
SdfAtlas& SdfAtlas::operator=(SdfAtlas&&) noexcept = default;

bool SdfAtlas::build(const std::string& /*font_family*/,
                     const std::vector<char32_t>& chars,
                     int base_size,
                     int padding,
                     int max_atlas_size) {
    if (chars.empty() || base_size <= 0 || padding < 0) return false;

    impl_ = std::make_unique<Impl>();
    impl_->base_size = base_size;
    impl_->padding   = padding;

    const int tile = base_size + 2 * padding;
    const int cols = std::max(1, max_atlas_size / tile);
    const int rows = (static_cast<int>(chars.size()) + cols - 1) / cols;
    impl_->width  = cols * tile;
    impl_->height = rows * tile;
    if (impl_->width > max_atlas_size || impl_->height > max_atlas_size) {
        impl_.reset();
        return false;
    }
    impl_->pixels.assign(static_cast<std::size_t>(impl_->width * impl_->height), 0);

    for (std::size_t i = 0; i < chars.size(); ++i) {
        const int col = static_cast<int>(i) % cols;
        const int row = static_cast<int>(i) / cols;
        const int x0 = col * tile;
        const int y0 = row * tile;

        // Rasterize a binary mask for this glyph (placeholder for now).
        auto mask = rasterize_placeholder(chars[i], tile, tile, padding);

        // Distance transform.
        auto dist = distance_transform(mask.data(), tile, tile);

        // Map to 0..255, 128 = edge.
        const float spread = static_cast<float>(padding);
        for (int y = 0; y < tile; ++y) {
            for (int x = 0; x < tile; ++x) {
                float d = dist[static_cast<std::size_t>(y * tile + x)];
                // Clamp to [-spread, +spread]
                if (d >  spread) d =  spread;
                if (d < -spread) d = -spread;
                // Inside is negative; we want inside = high values
                // (so smoothstep > 0.5 means inside).
                float t = 0.5f - 0.5f * (d / spread);
                int v = static_cast<int>(std::lround(t * 255.0f));
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                impl_->pixels[static_cast<std::size_t>((y0 + y) * impl_->width + (x0 + x))]
                    = static_cast<std::uint8_t>(v);
            }
        }

        SdfGlyph g;
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

const SdfGlyph* SdfAtlas::glyph(char32_t codepoint) const {
    if (!impl_) return nullptr;
    auto it = impl_->glyphs.find(codepoint);
    if (it == impl_->glyphs.end()) return nullptr;
    return &it->second;
}

const std::uint8_t* SdfAtlas::pixels() const {
    return impl_ ? impl_->pixels.data() : nullptr;
}

int SdfAtlas::width()  const { return impl_ ? impl_->width  : 0; }
int SdfAtlas::height() const { return impl_ ? impl_->height : 0; }

std::size_t SdfAtlas::glyph_count() const {
    return impl_ ? impl_->glyphs.size() : 0;
}

int SdfAtlas::base_size() const { return impl_ ? impl_->base_size : 0; }

} // namespace pulp::canvas
