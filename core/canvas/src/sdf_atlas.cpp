// SDF glyph atlas — exploration implementation for #76.
//
// Algorithm:
//   1. For each requested codepoint, rasterize a binary mask at
//      base_size + 2*padding. With Skia available
//      (PULP_HAS_SKIA), the mask comes from SkFont::drawText into an
//      offscreen SkBitmap. Without Skia, falls back to a placeholder
//      circle rasterizer so the test suite still runs.
//   2. Run a 2-pass Euclidean distance transform (Felzenszwalb &
//      Huttenlocher 2004) over the mask to produce signed distance
//      values in pixels.
//   3. Map the distance to [0, 255] with 128 = edge, clamped to a
//      range of [-padding, +padding] pixels.
//   4. Pack tiles into a single atlas image using a simple shelf
//      packer.

#include <pulp/canvas/sdf_atlas.hpp>
#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/font_options.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

#if PULP_HAS_SKIA
    #include "include/core/SkBitmap.h"
    #include "include/core/SkCanvas.h"
    #include "include/core/SkColor.h"
    #include "include/core/SkFont.h"
    #include "include/core/SkFontMgr.h"
    #include "include/core/SkPaint.h"
    #include "include/core/SkTypeface.h"
    #include "include/core/SkImageInfo.h"
    #ifdef __APPLE__
        #include "include/ports/SkFontMgr_mac_ct.h"
    #elif defined(_WIN32)
        #include "include/ports/SkTypeface_win.h"
    #elif defined(__ANDROID__)
        #include "include/ports/SkFontMgr_android.h"
        #include "include/ports/SkFontScanner_FreeType.h"
    #elif defined(__linux__)
        #include "include/ports/SkFontMgr_fontconfig.h"
        #include "include/ports/SkFontScanner_FreeType.h"
    #endif
#endif

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

// Real glyph metrics captured from SkFont. Replaces the placeholder
// values that were previously stored on SdfGlyph. Units are pixels at
// base_size.
struct GlyphMetrics {
    float bearing_x = 0.0f;  // left side bearing (positive = inset from origin)
    float bearing_y = 0.0f;  // top bearing above baseline (positive up)
    float advance   = 0.0f;  // pen advance after drawing
    bool  valid     = false;
};

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

#if PULP_HAS_SKIA
// pulp #2163 / font v2 Slice 1.1.a — platform font manager comes from
// the single canonical helper in bundled_fonts.cpp; this TU-local shim
// preserves the existing call sites until the broader caller-migration
// pass routes them through the resolver.
sk_sp<SkFontMgr> make_font_mgr() {
    return platform_font_manager();
}

sk_sp<SkTypeface> resolve_typeface(const std::string& font_family) {
    // pulp #2163 / font v2 Slice 1.1.a — was a platform-only
    // matchFamilyStyle (ignored plugin-registered + bundled). Now
    // routes through FontResolver so the SDF atlas path sees the
    // same cascade as Skia text rendering. Closes v1 doc gap §1.6.
    FontOptions opts;
    if (!font_family.empty()) opts.family_stack.push_back(font_family);
    auto resolved = FontResolver::instance().resolve_family_list(opts);
    if (resolved.typeface) return resolved.typeface;
    // Last-resort legacy default for environments where the resolver
    // cascade misses entirely (kept as a safety net; trace records
    // emitted by the resolver document the miss).
    auto mgr = make_font_mgr();
    if (!mgr) return nullptr;
    return mgr->legacyMakeTypeface(nullptr, SkFontStyle::Normal());
}

// Capture glyph metrics from SkFont at base_size. Returns valid=false
// when the font or glyph cannot be resolved.
GlyphMetrics glyph_metrics_skia(const sk_sp<SkTypeface>& face,
                                 char32_t codepoint,
                                 int base_size) {
    GlyphMetrics m;
    if (!face) return m;

    SkFont font(face, static_cast<SkScalar>(base_size));
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setHinting(SkFontHinting::kNone);
    font.setSubpixel(true);

    SkGlyphID gid = 0;
    {
        SkUnichar uni = static_cast<SkUnichar>(codepoint);
        // Use the SkSpan-taking overload: it is always available in
        // chrome/m144 and newer Skia, whereas the (ptr, count) overload
        // is gated behind SK_SUPPORT_UNSPANNED_APIS and disappears on
        // Skia builds that compile with that macro undefined. See #543.
        font.unicharsToGlyphs(SkSpan<const SkUnichar>(&uni, 1),
                              SkSpan<SkGlyphID>(&gid, 1));
    }
    if (gid == 0) return m;  // .notdef — still emit advance below

    SkScalar advance = 0;
    SkRect bounds{};
    // Same SkSpan migration for getWidthsBounds — see #543.
    font.getWidthsBounds(SkSpan<const SkGlyphID>(&gid, 1),
                         SkSpan<SkScalar>(&advance, 1),
                         SkSpan<SkRect>(&bounds, 1),
                         nullptr);

    m.advance = static_cast<float>(advance);
    // SkRect here is in device-space relative to the pen origin, y-down
    // (top is the most negative). Convert to Pulp's "bearing_y positive up":
    m.bearing_x = static_cast<float>(bounds.fLeft);
    m.bearing_y = static_cast<float>(-bounds.fTop);
    m.valid = true;
    return m;
}

// Skia-backed glyph rasterizer.
//
// Builds a temporary SkBitmap of size w × h, paints the requested
// codepoint into the centre using the requested font, and reads back
// the alpha channel as a binary mask. Returns an empty vector if the
// font cannot be loaded so the caller can fall back to the
// placeholder.
std::vector<std::uint8_t> rasterize_skia(const std::string& font_family,
                                          char32_t codepoint,
                                          int base_size,
                                          int w, int h, int padding) {
    // pulp #2163 / font v2 Slice 1.1.a — same migration as
    // resolve_typeface above: use FontResolver so plugin-registered
    // and bundled fonts are honored by the SDF rasterizer path.
    sk_sp<SkTypeface> face;
    {
        FontOptions opts;
        if (!font_family.empty()) opts.family_stack.push_back(font_family);
        face = FontResolver::instance().resolve_family_list(opts).typeface;
    }
    if (!face) {
        auto mgr = make_font_mgr();
        if (mgr) face = mgr->legacyMakeTypeface(nullptr, SkFontStyle::Normal());
    }
    if (!face) return {};

    SkFont font(face, static_cast<SkScalar>(base_size));
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setHinting(SkFontHinting::kNone);
    font.setSubpixel(false);

    SkBitmap bitmap;
    auto info = SkImageInfo::MakeA8(w, h);
    if (!bitmap.tryAllocPixels(info)) return {};
    bitmap.eraseColor(SK_ColorTRANSPARENT);

    SkCanvas canvas(bitmap);
    SkPaint paint;
    paint.setColor(SK_ColorWHITE);
    paint.setAntiAlias(true);

    // Convert codepoint → glyph and draw centered. We use drawSimpleText
    // with UTF-32 to dodge any UTF-8 encoding edge cases.
    char32_t cps[1] = { codepoint };
    SkScalar baseline_y = static_cast<SkScalar>(h - padding);
    SkScalar text_x = static_cast<SkScalar>(padding);
    canvas.drawSimpleText(cps, sizeof(cps),
                          SkTextEncoding::kUTF32,
                          text_x, baseline_y,
                          font, paint);

    // Read back the A8 plane as a binary mask. Any non-zero alpha
    // counts as inside — the distance transform handles antialiasing
    // implicitly via the smoothstep at sample time.
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(w * h), 0);
    const std::uint8_t* src = static_cast<const std::uint8_t*>(bitmap.getPixels());
    if (!src) return {};
    const int row_bytes = static_cast<int>(bitmap.rowBytes());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            mask[static_cast<std::size_t>(y * w + x)] =
                (src[y * row_bytes + x] > 32) ? 1 : 0;
        }
    }
    return mask;
}
#endif // PULP_HAS_SKIA

// Dispatcher: prefer Skia rasterization, fall back to the placeholder.
std::vector<std::uint8_t> rasterize_glyph(const std::string& font_family,
                                           char32_t codepoint,
                                           int base_size,
                                           int w, int h, int padding) {
#if PULP_HAS_SKIA
    auto mask = rasterize_skia(font_family, codepoint, base_size, w, h, padding);
    if (!mask.empty()) return mask;
#else
    (void)font_family;
    (void)base_size;
#endif
    return rasterize_placeholder(codepoint, w, h, padding);
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

bool SdfAtlas::build(const std::string& font_family,
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

#if PULP_HAS_SKIA
    auto shared_face = resolve_typeface(font_family);
#endif

    for (std::size_t i = 0; i < chars.size(); ++i) {
        const int col = static_cast<int>(i) % cols;
        const int row = static_cast<int>(i) / cols;
        const int x0 = col * tile;
        const int y0 = row * tile;

        // Rasterize a binary mask for this glyph. Uses Skia/SkFont
        // when PULP_HAS_SKIA is set; otherwise falls back to the
        // placeholder so the test suite still passes on hosts without
        // Skia.
        auto mask = rasterize_glyph(font_family, chars[i], base_size, tile, tile, padding);

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
        // Default (fallback) metrics if SkFont is unavailable.
        g.bearing_x = 0.0f;
        g.bearing_y = static_cast<float>(base_size);
        g.advance   = static_cast<float>(base_size);
#if PULP_HAS_SKIA
        auto m = glyph_metrics_skia(shared_face, chars[i], base_size);
        if (m.valid) {
            g.bearing_x = m.bearing_x;
            g.bearing_y = m.bearing_y;
            g.advance   = m.advance;
        }
#endif
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
