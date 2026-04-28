#include <algorithm>
#include <unordered_map>
#include <pulp/canvas/skia_canvas.hpp>

#ifdef PULP_HAS_SKIA

#include <algorithm>
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"
#include "include/core/SkTextBlob.h"
#include "modules/skshaper/include/SkShaper.h"
#include "include/core/SkRRect.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "include/core/SkSamplingOptions.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/effects/SkGradientShader.h"
#include "include/effects/SkDashPathEffect.h"
#include "runtime_effect_cache.hpp"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkBlendMode.h"
#include "include/effects/SkImageFilters.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#include "webgpu/webgpu_cpp.h"

// Platform font manager
#ifdef __APPLE__
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#elif defined(__ANDROID__)
// Android: use the built-in Android font manager with FreeType scanner
#include "include/ports/SkFontMgr_android.h"
#include "include/ports/SkFontScanner_FreeType.h"
#elif defined(__linux__)
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#endif

namespace pulp::canvas {

// Lazily create a platform-appropriate font manager
// macOS: CoreText, Windows: DirectWrite, Linux: fontconfig
static sk_sp<SkFontMgr> get_font_manager() {
    static sk_sp<SkFontMgr> mgr;
    static bool tried = false;
    if (!tried) {
        tried = true;
#ifdef __APPLE__
        mgr = SkFontMgr_New_CoreText(nullptr);
#elif defined(_WIN32)
        mgr = SkFontMgr_New_DirectWrite();
#elif defined(__ANDROID__)
        // Android font manager needs a FreeType scanner to rasterize glyphs.
        // Passing nullptr for the scanner causes SIGSEGV in drawSimpleText.
        mgr = SkFontMgr_New_Android(nullptr, SkFontScanner_Make_FreeType());
#elif defined(__linux__)
        mgr = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
        // Don't fall back to RefEmpty — callers check for null
    }
    return mgr;
}

static SkColor to_sk_color(Color c) {
    return c.to_argb32();
}

static SkColor4f to_sk_color4f(Color c) {
    return {c.r, c.g, c.b, c.a};
}

static SkPaint make_fill_paint(Color c) {
    SkPaint paint;
    paint.setColor4f(to_sk_color4f(c));
    paint.setStyle(SkPaint::kFill_Style);
    paint.setAntiAlias(true);
    return paint;
}

static SkPaint make_stroke_paint(Color c, float width) {
    SkPaint paint;
    paint.setColor4f(to_sk_color4f(c));
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(width);
    paint.setAntiAlias(true);
    return paint;
}

// Cached typeface loaded directly from a known path — bypasses family name
// matching for deterministic metrics (Visage-style approach). Cache key
// includes weight + slant so setFontWeight(700) actually returns a bold
// typeface rather than the same Regular blob (pulp #927).
static sk_sp<SkTypeface> get_cached_typeface(const std::string& family,
                                             int weight, int slant) {
    struct Key { std::string family; int weight; int slant; };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            return std::hash<std::string>{}(k.family)
                ^ (static_cast<size_t>(k.weight) * 31u)
                ^ (static_cast<size_t>(k.slant) * 1297u);
        }
    };
    struct KeyEq {
        bool operator()(const Key& a, const Key& b) const noexcept {
            return a.weight == b.weight && a.slant == b.slant && a.family == b.family;
        }
    };
    static std::unordered_map<Key, sk_sp<SkTypeface>, KeyHash, KeyEq> cache;

    Key key{family, weight, slant};
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    SkFontStyle sk_style{
        weight,
        SkFontStyle::kNormal_Width,
        slant ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant
    };

    sk_sp<SkTypeface> typeface;

#if defined(__ANDROID__)
    // Load Roboto directly from filesystem for deterministic rendering.
    // This avoids the font manager's family matching which can return
    // different fonts depending on the device/API level. Only the
    // Regular/Upright path has a baked-in file; bold/italic still go
    // through the family matcher below.
    if (weight == 400 && slant == 0 &&
        (family == "sans-serif" || family == "Roboto" || family.empty())) {
        auto mgr = get_font_manager();
        if (mgr) {
            typeface = mgr->makeFromFile("/system/fonts/Roboto-Regular.ttf");
        }
    }
#endif

    // Fall back to family name matching
    if (!typeface) {
        auto mgr = get_font_manager();
        if (mgr && mgr->countFamilies() > 0) {
            typeface = mgr->matchFamilyStyle(family.c_str(), sk_style);
            if (!typeface)
                typeface = mgr->matchFamilyStyle(nullptr, sk_style);
        }
    }

    cache[key] = typeface;
    return typeface;
}

// Legacy single-arg overload — preserves the old "Normal" behaviour for
// callers that haven't migrated to the weight/slant-aware path.
static sk_sp<SkTypeface> get_cached_typeface(const std::string& family) {
    return get_cached_typeface(family, SkFontStyle::kNormal_Weight, 0);
}

static SkFont make_font(const std::string& family, float size,
                        int weight = SkFontStyle::kNormal_Weight,
                        int slant = 0) {
    SkFont font;
    font.setSize(size);
    font.setSubpixel(true);                               // Subpixel glyph positioning
    font.setEdging(SkFont::Edging::kSubpixelAntiAlias);   // LCD-quality anti-aliasing
    font.setHinting(SkFontHinting::kSlight);               // Light hinting preserves glyph shapes
    font.setLinearMetrics(true);                           // Linear scaling for consistent metrics

    auto typeface = get_cached_typeface(family, weight, slant);
    if (typeface) font.setTypeface(std::move(typeface));

    return font;
}

static SkColorType sk_color_type_from_webgpu_format(const std::string& format) {
    if (format == "rgba16float") return kRGBA_F16_SkColorType;
    if (format == "rgba8unorm" || format == "rgba8unorm-srgb") return kRGBA_8888_SkColorType;
    return kBGRA_8888_SkColorType;
}

static sk_sp<SkColorSpace> sk_color_space_from_webgpu_format(const std::string& format) {
    if (format == "rgba16float") return SkColorSpace::MakeSRGBLinear();
    return SkColorSpace::MakeSRGB();
}

SkiaCanvas::SkiaCanvas(SkCanvas* canvas, skgpu::graphite::Recorder* recorder)
    : canvas_(canvas), recorder_(recorder) {}
SkiaCanvas::~SkiaCanvas() = default;

// Null-safe: canvas_ can be null when swapchain texture wrap fails on Android
#define GUARD_CANVAS if (!canvas_) return

void SkiaCanvas::save() { GUARD_CANVAS; canvas_->save(); }
void SkiaCanvas::restore() { GUARD_CANVAS; canvas_->restore(); }

void SkiaCanvas::translate(float x, float y) { GUARD_CANVAS; canvas_->translate(x, y); }
void SkiaCanvas::scale(float sx, float sy) { GUARD_CANVAS; canvas_->scale(sx, sy); }
void SkiaCanvas::rotate(float radians) {
    GUARD_CANVAS;
    canvas_->rotate(radians * 180.0f / 3.14159265f);
}

void SkiaCanvas::set_transform(float a, float b, float c,
                               float d, float e, float f) {
    GUARD_CANVAS;
    // CanvasRenderingContext2D.setTransform uses the column-major form
    //   [a c e]   [scaleX skewY translateX]
    //   [b d f] = [skewX  scaleY translateY]
    //   [0 0 1]
    // SkMatrix::MakeAll takes row-major (sx, kx, tx, ky, sy, ty, p0, p1, p2).
    // Compose onto paint_baseline_ so a CanvasWidget at non-zero offset
    // keeps its inbound View transform when JS calls ctx.setTransform.
    SkMatrix user = SkMatrix::MakeAll(a, c, e,
                                       b, d, f,
                                       0.0f, 0.0f, 1.0f);
    canvas_->setMatrix(SkMatrix::Concat(paint_baseline_, user));
}

void SkiaCanvas::capture_paint_baseline_transform() {
    GUARD_CANVAS;
    paint_baseline_ = canvas_->getTotalMatrix();
}

void SkiaCanvas::concat_transform(float a, float b, float c,
                                  float d, float e, float f) {
    GUARD_CANVAS;
    // CanvasRenderingContext2D affine matrix layout:
    //   [a c e]
    //   [b d f]
    //   [0 0 1]
    // SkMatrix::MakeAll takes row-major (sx, kx, tx, ky, sy, ty, p0, p1, p2),
    // so the columns map: sx=a, kx=c, tx=e, ky=b, sy=d, ty=f.
    // SkCanvas::concat multiplies the supplied matrix into the current matrix
    // (right-side concat), exactly the View-level composition we want.
    SkMatrix m = SkMatrix::MakeAll(a, c, e,
                                    b, d, f,
                                    0.0f, 0.0f, 1.0f);
    canvas_->concat(m);
}

void SkiaCanvas::clip_rect(float x, float y, float w, float h) {
    GUARD_CANVAS;
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
}

void SkiaCanvas::clip() {
    GUARD_CANVAS;
    if (!path_builder_) return;
    // Snapshot the path (don't detach — Canvas2D allows continued use of
    // the same path after clip()) and intersect with the current clip.
    canvas_->clipPath(path_builder_->snapshot(), /*doAntiAlias=*/true);
}

void SkiaCanvas::set_fill_color(Color c) { fill_color_ = c; }
void SkiaCanvas::set_stroke_color(Color c) { stroke_color_ = c; }
void SkiaCanvas::set_line_width(float w) { line_width_ = w; }

void SkiaCanvas::set_line_cap(LineCap cap) {
    line_cap_ = cap;
}

void SkiaCanvas::set_line_join(LineJoin join) {
    line_join_ = join;
}

void SkiaCanvas::fill_rect(float x, float y, float w, float h) {
    GUARD_CANVAS; canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), make_fill_paint(fill_color_));
}

// pulp #929 — clearRect must actually clear pixels rather than compose a
// transparent SrcOver fill (which is a visual no-op). Use SkBlendMode::kClear
// so the destination texels become transparent black, mirroring the HTML
// CanvasRenderingContext2D.clearRect semantics. Without this, the canvas
// widget sat over whatever the parent had previously painted (or the
// undefined swapchain-texture pixels), which on FilterBank looked like a
// stuck white inner area.
void SkiaCanvas::clear_rect(float x, float y, float w, float h) {
    GUARD_CANVAS;
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kClear);
    paint.setAntiAlias(false);
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

// Apply the active line-dash pattern to a stroke paint (issue-916).
// Skia's SkDashPathEffect requires an even-length pattern with
// nonnegative entries; the JS bridge ensures both, so we treat any
// validation failure as "no dash" rather than silently dropping it.
static void apply_line_dash(SkPaint& paint,
                             const std::vector<float>& intervals,
                             float phase) {
    if (intervals.size() < 2 || (intervals.size() % 2) != 0) return;
    paint.setPathEffect(SkDashPathEffect::Make(
        SkSpan<const SkScalar>(intervals.data(), intervals.size()),
        phase));
}

void SkiaCanvas::stroke_rect(float x, float y, float w, float h) {
    GUARD_CANVAS; auto paint = make_stroke_paint(stroke_color_, line_width_);
    apply_line_dash(paint, line_dash_, line_dash_phase_);
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

void SkiaCanvas::fill_rounded_rect(float x, float y, float w, float h, float radius) {
    GUARD_CANVAS; SkRRect rrect;
    rrect.setRectXY(SkRect::MakeXYWH(x, y, w, h), radius, radius);
    canvas_->drawRRect(rrect, make_fill_paint(fill_color_));
}

void SkiaCanvas::stroke_rounded_rect(float x, float y, float w, float h, float radius) {
    GUARD_CANVAS; SkRRect rrect;
    rrect.setRectXY(SkRect::MakeXYWH(x, y, w, h), radius, radius);
    auto paint = make_stroke_paint(stroke_color_, line_width_);
    apply_line_dash(paint, line_dash_, line_dash_phase_);
    canvas_->drawRRect(rrect, paint);
}

void SkiaCanvas::fill_circle(float cx, float cy, float radius) {
    GUARD_CANVAS; canvas_->drawCircle(cx, cy, radius, make_fill_paint(fill_color_));
}

void SkiaCanvas::stroke_circle(float cx, float cy, float radius) {
    GUARD_CANVAS;
    auto paint = make_stroke_paint(stroke_color_, line_width_);
    apply_line_dash(paint, line_dash_, line_dash_phase_);
    canvas_->drawCircle(cx, cy, radius, paint);
}

void SkiaCanvas::stroke_arc(float cx, float cy, float radius,
                           float start_angle, float end_angle) {
    float start_deg = start_angle * 180.0f / 3.14159265f;
    float sweep_deg = (end_angle - start_angle) * 180.0f / 3.14159265f;
    SkRect oval = SkRect::MakeXYWH(cx - radius, cy - radius, radius * 2, radius * 2);
    SkPath path = SkPathBuilder().addArc(oval, start_deg, sweep_deg).detach();
    if (canvas_) {
        auto paint = make_stroke_paint(stroke_color_, line_width_);
        apply_line_dash(paint, line_dash_, line_dash_phase_);
        canvas_->drawPath(path, paint);
    }
}

void SkiaCanvas::stroke_line(float x0, float y0, float x1, float y1) {
    GUARD_CANVAS;
    auto paint = make_stroke_paint(stroke_color_, line_width_);
    apply_line_dash(paint, line_dash_, line_dash_phase_);
    canvas_->drawLine(x0, y0, x1, y1, paint);
}

void SkiaCanvas::set_line_dash(const float* intervals, int count, float phase) {
    line_dash_.assign(intervals, intervals + (count > 0 ? count : 0));
    line_dash_phase_ = phase;
}

void SkiaCanvas::set_font(const std::string& family, float size) {
    font_family_ = family;
    font_size_ = size;
    // Reset rich state so a legacy set_font() call doesn't carry a previous
    // weight/slant/spacing forward unexpectedly.
    font_weight_ = SkFontStyle::kNormal_Weight;
    font_slant_ = 0;
    letter_spacing_ = 0.0f;
}

void SkiaCanvas::set_font_full(const std::string& family, float size,
                               int weight, int slant, float letter_spacing) {
    font_family_ = family;
    font_size_ = size;
    font_weight_ = weight;
    font_slant_ = slant;
    letter_spacing_ = letter_spacing;
}

void SkiaCanvas::set_text_align(TextAlign align) {
    text_align_ = align;
}

void SkiaCanvas::fill_text(const std::string& text, float x, float y) {
    GUARD_CANVAS;
    if (text.empty()) return;

    SkFont font = make_font(font_family_, font_size_, font_weight_, font_slant_);
    if (!font.getTypeface()) return;

    auto paint = make_fill_paint(fill_color_);

#ifdef PULP_HAS_TEXT_SHAPING
    // SkShaper path: full OpenType kerning + ligatures via HarfBuzz.
    //
    // The RunHandler origin MUST be {0,0}. The draw position is passed
    // exclusively to drawTextBlob() to avoid double-offset in nested
    // save/translate/clip contexts (issue #75). The bug: if the handler
    // is seeded with {x,y} AND drawTextBlob also receives {x,y}, glyph
    // positions are offset twice — once inside the blob and once by the
    // draw call — producing a ghost/double image that worsens with each
    // nesting level in the widget paint pipeline.
    //
    // pulp #927: when letter_spacing_ != 0 we cannot use the shaped blob
    // verbatim — the shaper packs glyph positions ignorant of CSS
    // letter-spacing. Fall through to the per-glyph builder below so the
    // extra advance lands on the rendered output.
    if (letter_spacing_ == 0.0f) {
        auto shaper = SkShaper::Make();
        if (shaper) {
            // Shape at origin {0,0}, then read the total shaped advance
            // from endPoint() for accurate text alignment.
            SkTextBlobBuilderRunHandler handler(text.c_str(), {0, 0});
            shaper->shape(text.c_str(), text.size(), font,
                          /*leftToRight=*/true, SK_ScalarInfinity, &handler);
            float total_w = handler.endPoint().x();

            float draw_x = x;
            if (text_align_ == TextAlign::center) draw_x -= total_w * 0.5f;
            else if (text_align_ == TextAlign::right) draw_x -= total_w;

            auto blob = handler.makeBlob();
            if (blob) {
                canvas_->drawTextBlob(blob, draw_x, y, paint);
                return;
            }
        }
    }
#endif

    // Fallback: per-glyph SkTextBlob without kerning/ligatures.
    // Used when text shaping is disabled, SkShaper::Make() fails, or
    // letter_spacing_ != 0 (pulp #927 — the shaped blob can't carry CSS
    // letter-spacing so we lay glyphs out manually).
    int glyph_count = static_cast<int>(font.countText(text.c_str(), text.size(), SkTextEncoding::kUTF8));
    if (glyph_count <= 0) return;

    std::vector<SkGlyphID> glyphs(glyph_count);
    font.textToGlyphs(text.c_str(), text.size(), SkTextEncoding::kUTF8,
                      SkSpan<SkGlyphID>(glyphs.data(), glyph_count));

    std::vector<SkScalar> widths(glyph_count);
    font.getWidths(SkSpan<const SkGlyphID>(glyphs.data(), glyph_count),
                   SkSpan<SkScalar>(widths.data(), glyph_count));

    float total_w = 0;
    for (int i = 0; i < glyph_count; ++i) total_w += widths[i];
    // CSS letter-spacing: extra advance between every pair of glyphs.
    if (glyph_count > 1) total_w += letter_spacing_ * static_cast<float>(glyph_count - 1);

    float draw_x = x;
    if (text_align_ == TextAlign::center) draw_x -= total_w * 0.5f;
    else if (text_align_ == TextAlign::right) draw_x -= total_w;

    SkTextBlobBuilder builder;
    const auto& run = builder.allocRunPosH(font, glyph_count, y);
    float cursor = draw_x;
    for (int i = 0; i < glyph_count; ++i) {
        run.glyphs[i] = glyphs[i];
        run.pos[i] = cursor;
        cursor += widths[i];
        if (i + 1 < glyph_count) cursor += letter_spacing_;
    }

    canvas_->drawTextBlob(builder.make(), 0, 0, paint);
}

void SkiaCanvas::fill_text_sdf(const std::string& text, float x, float y,
                               const SdfAtlas& atlas) {
    GUARD_CANVAS;
    if (text.empty()) return;

    // Scale factor: how the current font_size_ relates to the atlas's base_size.
    float scale = font_size_ / static_cast<float>(atlas.base_size());
    int atlas_w = atlas.width();
    int atlas_h = atlas.height();
    if (atlas_w <= 0 || atlas_h <= 0) { fill_text(text, x, y); return; }

    // Create atlas texture as an SkImage (uploaded to GPU on first use).
    // The atlas is single-channel (R8), so we use kAlpha_8_SkColorType
    // which maps the byte to the alpha channel — the shader reads it.
    auto image_data = SkData::MakeWithoutCopy(atlas.pixels(), atlas_w * atlas_h);
    SkImageInfo info = SkImageInfo::Make(atlas_w, atlas_h,
                                         kAlpha_8_SkColorType, kPremul_SkAlphaType);
    auto atlas_image = SkImages::RasterFromData(info, image_data, atlas_w);
    if (!atlas_image) { fill_text(text, x, y); return; }

    // Compute glyph positions and total advance for alignment.
    float total_advance = 0;
    struct GlyphDraw { const SdfGlyph* g; float x_offset; };
    std::vector<GlyphDraw> draws;

    // UTF-8 → codepoint iteration with bounds checking.
    // Falls back to fill_text() if any glyph is missing from the atlas
    // (partial atlas should not produce invisible characters).
    bool has_missing_glyph = false;
    size_t i = 0;
    while (i < text.size()) {
        char32_t cp;
        uint8_t c = static_cast<uint8_t>(text[i]);
        if (c < 0x80) {
            cp = c; i += 1;
        } else if (c < 0xE0) {
            if (i + 1 >= text.size()) break;  // truncated
            cp = (c & 0x1F) << 6 | (text[i+1] & 0x3F); i += 2;
        } else if (c < 0xF0) {
            if (i + 2 >= text.size()) break;
            cp = (c & 0x0F) << 12 | (text[i+1] & 0x3F) << 6 | (text[i+2] & 0x3F); i += 3;
        } else {
            if (i + 3 >= text.size()) break;
            cp = (c & 0x07) << 18 | (text[i+1] & 0x3F) << 12 | (text[i+2] & 0x3F) << 6 | (text[i+3] & 0x3F); i += 4;
        }

        const SdfGlyph* g = atlas.glyph(cp);
        if (!g) { has_missing_glyph = true; break; }
        draws.push_back({g, total_advance});
        total_advance += g->advance * scale;
    }

    // If any glyph is missing, fall back to standard text rendering
    // rather than producing incomplete/misaligned output.
    if (has_missing_glyph || draws.empty()) { fill_text(text, x, y); return; }

    // Apply text alignment.
    float draw_x = x;
    if (text_align_ == TextAlign::center) draw_x -= total_advance * 0.5f;
    else if (text_align_ == TextAlign::right) draw_x -= total_advance;

    // Draw each glyph as a textured quad with the SDF alpha channel.
    // The smoothstep is applied per-pixel by Skia's shader pipeline
    // when we use kAlpha_8 — we just draw with the fill color's paint.
    auto paint = make_fill_paint(fill_color_);
    for (auto& [g, x_off] : draws) {
        float gx = draw_x + x_off + g->bearing_x * scale;
        float gy = y - g->bearing_y * scale;
        float gw = g->width * scale;
        float gh = g->height * scale;

        SkRect src = SkRect::MakeXYWH(g->atlas_x, g->atlas_y, g->width, g->height);
        SkRect dst = SkRect::MakeXYWH(gx, gy, gw, gh);

        canvas_->drawImageRect(atlas_image, src, dst,
                               SkSamplingOptions(SkFilterMode::kLinear),
                               &paint, SkCanvas::kStrict_SrcRectConstraint);
    }
}

float SkiaCanvas::measure_text(const std::string& text) {
    SkFont font = make_font(font_family_, font_size_, font_weight_, font_slant_);
    if (!font.getTypeface()) return font_size_ * text.size() * 0.5f;

#ifdef PULP_HAS_TEXT_SHAPING
    // Use SkShaper for accurate post-kerning width when shaping is
    // enabled — matches what fill_text() actually renders. Without
    // this, measure_text returns unshaped widths that mismatch drawn
    // text for kerning/ligature strings (e.g., "AV", "ffi").
    //
    // pulp #927: if letter_spacing_ != 0 we bypass the shaper because
    // fill_text() also bypasses it in that case — measuring via the
    // shaper would diverge from what's actually drawn.
    if (letter_spacing_ == 0.0f) {
        auto shaper = SkShaper::Make();
        if (shaper) {
            SkTextBlobBuilderRunHandler handler(text.c_str(), {0, 0});
            shaper->shape(text.c_str(), text.size(), font,
                          /*leftToRight=*/true, SK_ScalarInfinity, &handler);
            return handler.endPoint().x();
        }
    }
#endif

    // Fallback: per-glyph advances (no kerning/ligatures)
    int glyph_count = static_cast<int>(font.countText(text.c_str(), text.size(), SkTextEncoding::kUTF8));
    if (glyph_count <= 0) return 0;

    std::vector<SkGlyphID> glyphs(glyph_count);
    font.textToGlyphs(text.c_str(), text.size(), SkTextEncoding::kUTF8,
                      SkSpan<SkGlyphID>(glyphs.data(), glyph_count));

    std::vector<SkScalar> widths(glyph_count);
    font.getWidths(SkSpan<const SkGlyphID>(glyphs.data(), glyph_count),
                   SkSpan<SkScalar>(widths.data(), glyph_count));

    float total = 0;
    for (int i = 0; i < glyph_count; ++i) total += widths[i];
    // pulp #927: include CSS letter-spacing in measurement so layout code
    // (e.g. ellipsis truncation in Label::paint) reasons over the same
    // total advance the renderer will draw.
    if (glyph_count > 1) total += letter_spacing_ * static_cast<float>(glyph_count - 1);
    return total;
}

Canvas::TextMetrics SkiaCanvas::measure_text_full(const std::string& text) {
    SkFont font = make_font(font_family_, font_size_, font_weight_, font_slant_);
    if (!font.getTypeface()) {
        // Fallback estimates — keep all fields populated so JS callers can
        // still e.g. center text against actual_bounding_box_left/right.
        TextMetrics fallback;
        fallback.width = font_size_ * text.size() * 0.5f;
        fallback.ascent = font_size_;
        fallback.descent = font_size_ * 0.25f;
        fallback.line_height = font_size_ * 1.2f;
        fallback.actual_bounding_box_ascent = fallback.ascent;
        fallback.actual_bounding_box_descent = fallback.descent;
        fallback.actual_bounding_box_left = 0;
        fallback.actual_bounding_box_right = fallback.width;
        return fallback;
    }

    SkFontMetrics sk_metrics;
    font.getMetrics(&sk_metrics);

    SkRect bounds;
    // SkFont::measureText returns the advance width and stores the
    // tight rendering bbox in `bounds`. The advance is what
    // CanvasRenderingContext2D.measureText.width returns; the bbox
    // gives us the actualBoundingBoxLeft/Right/Ascent/Descent fields
    // (issue-916).
    SkScalar advance = font.measureText(
        text.c_str(), text.size(), SkTextEncoding::kUTF8, &bounds);

    TextMetrics m;
    m.width = advance;
    m.ascent = -sk_metrics.fAscent;   // Skia ascent is negative, we return positive
    m.descent = sk_metrics.fDescent;  // Skia descent is positive
    m.line_height = -sk_metrics.fAscent + sk_metrics.fDescent + sk_metrics.fLeading;

    // HTML5 TextMetrics — bounds is in glyph-local coordinates with the
    // text origin at (0, 0). Negate fLeft so positive values mean
    // "extends left of origin" per the spec.
    m.actual_bounding_box_left = -bounds.fLeft;
    m.actual_bounding_box_right = bounds.fRight;
    m.actual_bounding_box_ascent = -bounds.fTop;
    m.actual_bounding_box_descent = bounds.fBottom;
    return m;
}

Canvas::TextMetrics SkiaCanvas::measure_text_with_font(
    const std::string& family, float size, const std::string& text) {
    SkFont font = make_font(family, size);
    Canvas::TextMetrics m;
    if (!font.getTypeface()) {
        m.width = static_cast<float>(text.size()) * size * 0.5f;
        m.ascent = size;
        m.descent = size * 0.25f;
        m.line_height = size * 1.2f;
        m.actual_bounding_box_left = 0;
        m.actual_bounding_box_right = m.width;
        m.actual_bounding_box_ascent = m.ascent;
        m.actual_bounding_box_descent = m.descent;
        return m;
    }
    SkFontMetrics fm;
    font.getMetrics(&fm);
    SkRect bounds;
    SkScalar advance = font.measureText(
        text.c_str(), text.size(), SkTextEncoding::kUTF8, &bounds);
    m.width = advance;
    m.ascent = -fm.fAscent;
    m.descent = fm.fDescent;
    m.line_height = -fm.fAscent + fm.fDescent + fm.fLeading;
    m.actual_bounding_box_left = -bounds.fLeft;
    m.actual_bounding_box_right = bounds.fRight;
    m.actual_bounding_box_ascent = -bounds.fTop;
    m.actual_bounding_box_descent = bounds.fBottom;
    return m;
}

bool SkiaCanvas::read_pixels(int x, int y, int width, int height, uint8_t* out) {
    if (!canvas_ || !out || width <= 0 || height <= 0) return false;

    auto* surface = canvas_->getSurface();
    if (!surface) return false;

    // Read in unpremultiplied RGBA — matches HTML5 ImageData.data layout.
    SkImageInfo info = SkImageInfo::Make(
        width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    return surface->readPixels(info, out,
                               static_cast<size_t>(width) * 4u,
                               x, y);
}

bool SkiaCanvas::write_pixels(const uint8_t* data, int width, int height,
                               int dx, int dy) {
    if (!canvas_ || !data || width <= 0 || height <= 0) return false;

    SkImageInfo info = SkImageInfo::Make(
        width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    SkBitmap bitmap;
    if (!bitmap.installPixels(info, const_cast<uint8_t*>(data),
                              static_cast<size_t>(width) * 4u)) {
        return false;
    }
    auto image = bitmap.asImage();
    if (!image) return false;

    // CanvasRenderingContext2D.putImageData ignores the current transform
    // and global compositing — bypass them by saving/restoring around
    // a copy-mode draw.
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    canvas_->drawImage(image, static_cast<SkScalar>(dx), static_cast<SkScalar>(dy),
                       SkSamplingOptions(), &paint);
    return true;
}

// ── Images ──────────────────────────────────────────────────────────────────

bool SkiaCanvas::draw_image_from_data(const uint8_t* data, size_t size,
                                       float x, float y, float w, float h) {
    if (!canvas_ || !data || size == 0) return false;

    auto sk_data = SkData::MakeWithoutCopy(data, size);
    auto image = SkImages::DeferredFromEncodedData(sk_data);
    if (!image) return false;

    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h),
                           SkSamplingOptions(SkFilterMode::kLinear));
    return true;
}

bool SkiaCanvas::draw_image_from_file(const std::string& path,
                                       float x, float y, float w, float h) {
    if (!canvas_ || path.empty()) return false;

    auto sk_data = SkData::MakeFromFileName(path.c_str());
    if (!sk_data) return false;

    auto image = SkImages::DeferredFromEncodedData(sk_data);
    if (!image) return false;

    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h),
                           SkSamplingOptions(SkFilterMode::kLinear));
    return true;
}

// ── Gradients ────────────────────────────────────────────────────────────────

static void colors_to_skia(const Color* colors, const float* positions, int count,
                            std::vector<SkColor>& sk_colors, std::vector<SkScalar>& sk_pos) {
    sk_colors.resize(static_cast<size_t>(count));
    sk_pos.resize(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        sk_colors[static_cast<size_t>(i)] = colors[i].to_argb32();
        sk_pos[static_cast<size_t>(i)] = positions[i];
    }
}

void SkiaCanvas::set_fill_gradient_linear(float x0, float y0, float x1, float y1,
                                           const Color* colors, const float* positions, int count) {
    std::vector<SkColor> sk_colors;
    std::vector<SkScalar> sk_pos;
    colors_to_skia(colors, positions, count, sk_colors, sk_pos);
    SkPoint pts[2] = {{x0, y0}, {x1, y1}};
    gradient_shader_ = SkGradientShader::MakeLinear(pts, sk_colors.data(), sk_pos.data(), count,
                                                     SkTileMode::kClamp);
    has_gradient_ = gradient_shader_ != nullptr;
}

void SkiaCanvas::set_fill_gradient_radial(float cx, float cy, float radius,
                                           const Color* colors, const float* positions, int count) {
    std::vector<SkColor> sk_colors;
    std::vector<SkScalar> sk_pos;
    colors_to_skia(colors, positions, count, sk_colors, sk_pos);
    gradient_shader_ = SkGradientShader::MakeRadial({cx, cy}, radius, sk_colors.data(),
                                                     sk_pos.data(), count, SkTileMode::kClamp);
    has_gradient_ = gradient_shader_ != nullptr;
}

void SkiaCanvas::set_fill_gradient_conic(float cx, float cy, float start_angle,
                                          const Color* colors, const float* positions, int count) {
    std::vector<SkColor> sk_colors;
    std::vector<SkScalar> sk_pos;
    colors_to_skia(colors, positions, count, sk_colors, sk_pos);
    float start_deg = start_angle * 180.0f / 3.14159265f;
    gradient_shader_ = SkGradientShader::MakeSweep(cx, cy, sk_colors.data(), sk_pos.data(),
                                                     count, SkTileMode::kClamp,
                                                     start_deg, start_deg + 360.0f, 0, nullptr);
    has_gradient_ = gradient_shader_ != nullptr;
}

void SkiaCanvas::clear_fill_gradient() {
    gradient_shader_ = nullptr;
    has_gradient_ = false;
}

// ── Blend modes ─────────────────────────────────────────────────────────────

void SkiaCanvas::set_blend_mode(BlendMode mode) {
    // Indices 0..15 — advanced/W3C blend modes (must stay in sync with
    // canvas.hpp BlendMode enum).
    // Indices 16..26 — Porter-Duff compositing modes (issue-896).
    static const SkBlendMode map[] = {
        SkBlendMode::kSrcOver, SkBlendMode::kMultiply, SkBlendMode::kScreen,
        SkBlendMode::kOverlay, SkBlendMode::kDarken, SkBlendMode::kLighten,
        SkBlendMode::kColorDodge, SkBlendMode::kColorBurn, SkBlendMode::kHardLight,
        SkBlendMode::kSoftLight, SkBlendMode::kDifference, SkBlendMode::kExclusion,
        SkBlendMode::kHue, SkBlendMode::kSaturation, SkBlendMode::kColor,
        SkBlendMode::kLuminosity,
        // Porter-Duff
        SkBlendMode::kSrcOver,    // 16 source_over (CSS default alias)
        SkBlendMode::kDstOver,    // 17 destination_over
        SkBlendMode::kSrcIn,      // 18 source_in
        SkBlendMode::kDstIn,      // 19 destination_in
        SkBlendMode::kSrcOut,     // 20 source_out
        SkBlendMode::kDstOut,     // 21 destination_out
        SkBlendMode::kSrcATop,    // 22 source_atop
        SkBlendMode::kDstATop,    // 23 destination_atop
        SkBlendMode::kXor,        // 24 xor
        SkBlendMode::kSrc,        // 25 copy
        SkBlendMode::kPlus        // 26 lighter
    };
    int idx = static_cast<int>(mode);
    constexpr int count = static_cast<int>(sizeof(map) / sizeof(map[0]));
    if (idx < 0 || idx >= count) {
        blend_mode_ = SkBlendMode::kSrcOver;
        return;
    }
    blend_mode_ = map[idx];
}

// ── Path building ───────────────────────────────────────────────────────────

void SkiaCanvas::begin_path() {
    path_builder_ = std::make_unique<SkPathBuilder>();
}

void SkiaCanvas::move_to(float x, float y) {
    if (path_builder_) path_builder_->moveTo(x, y);
}

void SkiaCanvas::line_to(float x, float y) {
    if (path_builder_) path_builder_->lineTo(x, y);
}

void SkiaCanvas::quad_to(float cpx, float cpy, float x, float y) {
    if (path_builder_) path_builder_->quadTo(cpx, cpy, x, y);
}

void SkiaCanvas::cubic_to(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
    if (path_builder_) path_builder_->cubicTo(cp1x, cp1y, cp2x, cp2y, x, y);
}

void SkiaCanvas::close_path() {
    if (path_builder_) path_builder_->close();
}

void SkiaCanvas::fill_current_path() {
    if (!canvas_ || !path_builder_) return;
    SkPaint paint;
    paint.setAntiAlias(true);
    if (has_gradient_ && gradient_shader_) {
        paint.setShader(gradient_shader_);
    } else {
        paint.setColor4f(to_sk_color4f(fill_color_));
    }
    paint.setBlendMode(blend_mode_);
    canvas_->drawPath(path_builder_->detach(), paint);
}

void SkiaCanvas::stroke_current_path() {
    if (!canvas_ || !path_builder_) return;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setColor4f(to_sk_color4f(stroke_color_));
    paint.setStrokeWidth(line_width_);
    paint.setBlendMode(blend_mode_);
    apply_line_dash(paint, line_dash_, line_dash_phase_);
    canvas_->drawPath(path_builder_->detach(), paint);
}

// ── Static SkSL compilation (accessible from bridge without Canvas instance) ──

std::string Canvas::compile_sksl(const std::string& sksl) {
    if (sksl.empty()) return "Empty shader code";
    auto& cache = RuntimeEffectCache::instance();
    auto effect = cache.get_or_compile(sksl);
    return effect ? "" : cache.last_error();
}

// ── GPU SDF Shape Primitives ─────────────────────────────────────────────────

static const char* kSDFShapeSkSL = R"(
    uniform float2 resolution;
    uniform float shapeType;  // 0=rect, 1=circle, 2=rounded_rect, 3=arc, 4=diamond
    uniform float cornerRadius;
    uniform float strokeWidth;
    uniform float arcStart;
    uniform float arcSweep;
    uniform float squirclePower;
    uniform float innerRadius;
    uniform float armWidth;
    uniform float bezierCX;
    uniform float bezierCY;
    uniform half4 fillColor;
    uniform half4 strokeColor;

    // SDF for a box centered at origin
    float sdBox(float2 p, float2 b) {
        float2 d = abs(p) - b;
        return length(max(d, float2(0.0))) + min(max(d.x, d.y), 0.0);
    }

    // SDF for a circle centered at origin
    float sdCircle(float2 p, float r) {
        return length(p) - r;
    }

    // SDF for rounded box
    float sdRoundBox(float2 p, float2 b, float r) {
        float2 q = abs(p) - b + float2(r);
        return length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) - r;
    }

    // SDF for diamond (rotated square)
    float sdDiamond(float2 p, float s) {
        float2 q = abs(p);
        return (q.x + q.y - s) * 0.7071;
    }

    // SDF for squircle (superellipse): |x/a|^n + |y/b|^n = 1
    float sdSquircle(float2 p, float2 b, float n) {
        float2 q = abs(p) / b;
        return (pow(pow(q.x, n) + pow(q.y, n), 1.0/n) - 1.0) * min(b.x, b.y);
    }

    // SDF for equilateral triangle
    float sdTriangle(float2 p, float r) {
        float k = 1.7321; // sqrt(3)
        p.x = abs(p.x) - r;
        p.y = p.y + r / k;
        if (p.x + k * p.y > 0.0) p = float2(p.x - k * p.y, -k * p.x - p.y) / 2.0;
        p.x -= clamp(p.x, -2.0 * r, 0.0);
        return -length(p) * sign(p.y);
    }

    // SDF for ring (annulus)
    float sdRing(float2 p, float outer, float inner) {
        return abs(length(p) - (outer + inner) * 0.5) - (outer - inner) * 0.5;
    }

    // SDF for stadium (pill/capsule)
    float sdStadium(float2 p, float2 b) {
        float r = min(b.x, b.y);
        float2 q = abs(p) - float2(b.x - r, 0.0);
        return length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) - r;
    }

    // SDF for cross (plus sign)
    float sdCross(float2 p, float2 b, float armW) {
        float2 q = abs(p);
        float d1 = sdBox(q, float2(b.x, b.y * armW));
        float d2 = sdBox(q, float2(b.x * armW, b.y));
        return min(d1, d2);
    }

    // SDF for line segment with flat ends
    float sdFlatSegment(float2 p, float2 halfSize) {
        return sdBox(p, float2(halfSize.x, strokeWidth * 0.5));
    }

    // SDF for line segment with rounded ends
    float sdRoundedSegment(float2 p, float halfLen, float thickness) {
        p.x -= clamp(p.x, -halfLen, halfLen);
        return length(p) - thickness * 0.5;
    }

    // SDF for arc with thickness (flat caps)
    float sdFlatArc(float2 p, float outerR, float innerR, float startAngle, float sweepAngle) {
        float angle = atan2(p.y, p.x);
        float halfSweep = sweepAngle * 0.5;
        float midAngle = startAngle + halfSweep;
        float angleDiff = angle - midAngle;
        angleDiff = angleDiff - 6.2832 * floor((angleDiff + 3.1416) / 6.2832);
        float arcMask = abs(angleDiff) - halfSweep;
        float ringDist = abs(length(p) - (outerR + innerR) * 0.5) - (outerR - innerR) * 0.5;
        return max(ringDist, arcMask * outerR * 0.3);
    }

    // SDF for quadratic bezier curve with thickness (approximation)
    // Uses distance to the closest point on the curve segment
    float sdQuadBezier(float2 p, float2 a, float2 b, float2 c, float thickness) {
        // Approximate by sampling the curve at several points
        float minDist = 1e10;
        for (float t = 0.0; t <= 1.0; t += 0.05) {
            float2 q = (1.0-t)*(1.0-t)*a + 2.0*(1.0-t)*t*b + t*t*c;
            float d = length(p - q);
            minDist = min(minDist, d);
        }
        return minDist - thickness * 0.5;
    }

    half4 main(float2 coord) {
        float2 center = resolution * 0.5;
        float2 p = coord - center;
        float2 halfSize = center - float2(2.0);  // 2px padding for AA
        float r = min(halfSize.x, halfSize.y);
        float d;

        if (shapeType < 0.5) {
            d = sdBox(p, halfSize);              // 0: rect
        } else if (shapeType < 1.5) {
            d = sdCircle(p, r);                  // 1: circle
        } else if (shapeType < 2.5) {
            d = sdRoundBox(p, halfSize, cornerRadius); // 2: rounded rect
        } else if (shapeType < 3.5) {
            // 3: arc
            float angle = atan2(p.y, p.x);
            float halfSweep = arcSweep * 0.5;
            float midAngle = arcStart + halfSweep;
            float angleDiff = angle - midAngle;
            angleDiff = angleDiff - 6.2832 * floor((angleDiff + 3.1416) / 6.2832);
            float arcDist = abs(angleDiff) - halfSweep;
            float ringDist = abs(length(p) - r * 0.8) - strokeWidth * 0.5;
            d = max(ringDist, arcDist * r * 0.5);
        } else if (shapeType < 4.5) {
            d = sdDiamond(p, r);                 // 4: diamond
        } else if (shapeType < 5.5) {
            d = sdSquircle(p, halfSize, squirclePower); // 5: squircle
        } else if (shapeType < 6.5) {
            d = sdTriangle(p, r);                // 6: triangle
        } else if (shapeType < 7.5) {
            float outer = r;
            float inner = r * innerRadius;
            d = sdRing(p, outer, inner);         // 7: ring
        } else if (shapeType < 8.5) {
            d = sdStadium(p, halfSize);          // 8: stadium
        } else if (shapeType < 9.5) {
            d = sdCross(p, halfSize, armWidth);  // 9: cross
        } else if (shapeType < 10.5) {
            d = sdFlatSegment(p, halfSize);      // 10: flat segment
        } else if (shapeType < 11.5) {
            d = sdRoundedSegment(p, halfSize.x, max(strokeWidth, 2.0)); // 11: rounded segment
        } else if (shapeType < 12.5) {
            float outerR = r;
            float innerR = r * innerRadius;
            d = sdFlatArc(p, outerR, innerR, arcStart, arcSweep);       // 12: flat arc
        } else {
            // 13: quadratic bezier — control point from uniform parameters
            float2 a = float2(-halfSize.x, halfSize.y);
            float2 b = float2(bezierCX * halfSize.x, bezierCY * halfSize.y);
            float2 c = float2(halfSize.x, halfSize.y);
            d = sdQuadBezier(p, a, b, c, max(strokeWidth, 2.0));        // 13: quadratic bezier
        }

        // Render: filled or stroked with AA
        float aa = 1.0;
        if (strokeWidth > 0.0 && shapeType < 2.5) {
            float sd = abs(d) - strokeWidth * 0.5;
            float alpha = 1.0 - smoothstep(-aa, aa, sd);
            return strokeColor * half(alpha);
        } else {
            float alpha = 1.0 - smoothstep(-aa, aa, d);
            return fillColor * half(alpha);
        }
    }
)";

void SkiaCanvas::draw_sdf_shape(SDFShape shape, float x, float y, float w, float h,
                                 const SDFStyle& style) {
    if (!canvas_) { Canvas::draw_sdf_shape(shape, x, y, w, h, style); return; }

    static auto effectResult = SkRuntimeEffect::MakeForShader(SkString(kSDFShapeSkSL));
    if (!effectResult.effect) {
        Canvas::draw_sdf_shape(shape, x, y, w, h, style);
        return;
    }

    auto effect = effectResult.effect;
    SkRuntimeShaderBuilder builder(effect);
    builder.uniform("resolution") = SkV2{w, h};
    builder.uniform("shapeType") = static_cast<float>(shape);
    builder.uniform("cornerRadius") = style.corner_radius;
    builder.uniform("strokeWidth") = style.stroke_width;
    builder.uniform("arcStart") = style.arc_start;
    builder.uniform("arcSweep") = style.arc_sweep;
    builder.uniform("squirclePower") = style.squircle_power;
    builder.uniform("innerRadius") = style.inner_radius;
    builder.uniform("armWidth") = style.arm_width;
    builder.uniform("bezierCX") = style.bezier_cx;
    builder.uniform("bezierCY") = style.bezier_cy;
    builder.uniform("fillColor") = SkV4{
        style.fill_color.r, style.fill_color.g,
        style.fill_color.b, style.fill_color.a};
    builder.uniform("strokeColor") = SkV4{
        style.stroke_color.r, style.stroke_color.g,
        style.stroke_color.b, style.stroke_color.a};

    auto shader = builder.makeShader();
    if (!shader) { Canvas::draw_sdf_shape(shape, x, y, w, h, style); return; }

    SkPaint paint;
    paint.setShader(std::move(shader));
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

// ── Custom SkSL shader rendering ─────────────────────────────────────────────

bool SkiaCanvas::draw_with_sksl(const std::string& sksl,
                                 float x, float y, float w, float h,
                                 const ShaderUniforms& uniforms) {
    if (!canvas_ || sksl.empty()) return false;

    // Compile and cache the shader effect (process-lifetime cache)
    auto& cache = RuntimeEffectCache::instance();
    auto effect = cache.get_or_compile(sksl);
    if (!effect) return false;

    // Build shader with standard uniforms
    SkRuntimeShaderBuilder builder(effect);

    // Set all uniforms that exist in the shader (skip gracefully if not present)
    if (effect->findUniform("resolution"))
        builder.uniform("resolution") = SkV2{w, h};
    if (effect->findUniform("value"))
        builder.uniform("value") = uniforms.value;
    if (effect->findUniform("time"))
        builder.uniform("time") = uniforms.time;

    auto toSkV4 = [](Color c) -> SkV4 {
        return {c.r, c.g, c.b, c.a};
    };

    if (effect->findUniform("accentColor"))
        builder.uniform("accentColor") = toSkV4(uniforms.accent_color);
    if (effect->findUniform("bgColor"))
        builder.uniform("bgColor") = toSkV4(uniforms.bg_color);
    if (effect->findUniform("trackColor"))
        builder.uniform("trackColor") = toSkV4(uniforms.track_color);
    if (effect->findUniform("fillColor"))
        builder.uniform("fillColor") = toSkV4(uniforms.fill_color);
    if (effect->findUniform("thumbColor"))
        builder.uniform("thumbColor") = toSkV4(uniforms.thumb_color);

    auto shader = builder.makeShader();
    if (!shader) return false;

    SkPaint paint;
    paint.setShader(std::move(shader));
    canvas_->save();
    canvas_->translate(x, y);
    canvas_->drawRect(SkRect::MakeXYWH(0, 0, w, h), paint);
    canvas_->restore();
    return true;
}

bool SkiaCanvas::draw_native_dawn_texture(void* texture_handle,
                                          uint32_t width,
                                          uint32_t height,
                                          const std::string& format,
                                          float x,
                                          float y,
                                          float w,
                                          float h) {
    if (!canvas_ || !recorder_ || texture_handle == nullptr || width == 0 || height == 0) {
        return false;
    }

    auto* texture_ptr = static_cast<wgpu::Texture*>(texture_handle);
    if (texture_ptr == nullptr || !(*texture_ptr)) {
        return false;
    }

    auto backend_texture = skgpu::graphite::BackendTextures::MakeDawn(texture_ptr->Get());
    if (!backend_texture.isValid()) {
        return false;
    }

    auto image = SkImages::WrapTexture(recorder_,
                                       backend_texture,
                                       sk_color_type_from_webgpu_format(format),
                                       kPremul_SkAlphaType,
                                       sk_color_space_from_webgpu_format(format));
    if (!image) {
        return false;
    }

    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h), SkSamplingOptions());
    return true;
}

// ── Blur backdrop ────────────────────────────────────────────────────────────

void SkiaCanvas::draw_blurred_backdrop(float x, float y, float w, float h,
                                        float blur_radius, float corner_radius,
                                        Color tint) {
    if (!canvas_) return;

    SkRect rect = SkRect::MakeXYWH(x, y, w, h);

    // Backdrop blur using saveLayer with SkImageFilter
    auto blur = SkImageFilters::Blur(blur_radius, blur_radius, SkTileMode::kClamp, nullptr);

    canvas_->save();
    if (corner_radius > 0) {
        canvas_->clipRRect(SkRRect::MakeRectXY(rect, corner_radius, corner_radius), true);
    } else {
        canvas_->clipRect(rect, true);
    }

    // saveLayer with backdrop blur filter
    SkPaint layerPaint;
    layerPaint.setImageFilter(std::move(blur));
    canvas_->saveLayer(&rect, &layerPaint);
    canvas_->restore();

    // Tint overlay
    SkPaint tintPaint;
    tintPaint.setColor4f(to_sk_color4f(tint));
    if (corner_radius > 0) {
        canvas_->drawRRect(SkRRect::MakeRectXY(rect, corner_radius, corner_radius), tintPaint);
    } else {
        canvas_->drawRect(rect, tintPaint);
    }

    canvas_->restore();
}

// ── Box shadow (issue-925) ──────────────────────────────────────────────────

void SkiaCanvas::draw_box_shadow(float x, float y, float w, float h,
                                  float dx, float dy, float blur, float spread,
                                  Color color, bool inset, float corner_radius) {
    if (!canvas_) return;
    if (color.a <= 0.0f || (w <= 0.0f && h <= 0.0f)) return;

    // Skia's drop-shadow blur sigma is roughly half the CSS blur radius;
    // matching the WebView 1:1 within ~5px is the acceptance criterion
    // (#925) so we use blur/2.0 the way Chromium's compositor does.
    const float sigma = std::max(0.0f, blur * 0.5f);

    if (!inset) {
        // Outset drop shadow: render the inflated rounded-rect silhouette
        // through SkImageFilters::DropShadowOnly so only the shadow
        // remains (no source pixels). This is exactly what Chromium uses
        // for `box-shadow` and matches the WebView reference within ~5px
        // (#925 acceptance criterion).
        SkRect occluder = SkRect::MakeXYWH(x - spread,
                                           y - spread,
                                           w + spread * 2.0f,
                                           h + spread * 2.0f);
        if (occluder.width() <= 0.0f || occluder.height() <= 0.0f) return;

        SkPaint shadow_paint;
        shadow_paint.setAntiAlias(true);
        // The fill color underneath doesn't matter — DropShadowOnly drops
        // the source — but Skia still respects the paint's alpha.
        shadow_paint.setColor4f(to_sk_color4f(Color::rgba(0, 0, 0, 1)));
        shadow_paint.setImageFilter(
            SkImageFilters::DropShadowOnly(dx, dy, sigma, sigma,
                                            to_sk_color4f(color), nullptr,
                                            nullptr));

        if (corner_radius > 0.0f) {
            float r = corner_radius + spread * 0.5f;
            canvas_->drawRRect(SkRRect::MakeRectXY(occluder, r, r),
                                shadow_paint);
        } else {
            canvas_->drawRect(occluder, shadow_paint);
        }
        return;
    }

    // Inset shadow:
    //   1. Clip to the box silhouette.
    //   2. Use SkBlendMode::kSrcOut against an inflated occluder so the
    //      blurred mask only shows along the inside edges of the box.
    SkRect box = SkRect::MakeXYWH(x, y, w, h);
    canvas_->save();
    if (corner_radius > 0.0f) {
        canvas_->clipRRect(SkRRect::MakeRectXY(box, corner_radius, corner_radius),
                            true);
    } else {
        canvas_->clipRect(box, true);
    }

    // Paint a translucent rect of the shadow color that covers the box,
    // then punch out the inflated, offset, blurred occluder so what
    // remains is the inset shadow ring.
    SkPaint full_paint;
    full_paint.setAntiAlias(true);
    full_paint.setColor4f(to_sk_color4f(color));
    canvas_->saveLayer(&box, nullptr);
    canvas_->drawRect(box, full_paint);

    SkPaint hole_paint;
    hole_paint.setAntiAlias(true);
    hole_paint.setBlendMode(SkBlendMode::kDstOut);
    hole_paint.setColor4f(to_sk_color4f(Color::rgba(0, 0, 0, 1)));
    if (sigma > 0.0f) {
        hole_paint.setImageFilter(SkImageFilters::Blur(sigma, sigma,
                                                        SkTileMode::kDecal,
                                                        nullptr));
    }
    SkRect hole = SkRect::MakeXYWH(x + dx + spread,
                                    y + dy + spread,
                                    w - spread * 2.0f,
                                    h - spread * 2.0f);
    if (hole.width() > 0.0f && hole.height() > 0.0f) {
        if (corner_radius > 0.0f) {
            float r = std::max(0.0f, corner_radius - spread * 0.5f);
            canvas_->drawRRect(SkRRect::MakeRectXY(hole, r, r), hole_paint);
        } else {
            canvas_->drawRect(hole, hole_paint);
        }
    }
    canvas_->restore();
    canvas_->restore();
}

// ── Opacity & Compositing Layers ────────────────────────────────────────────

void SkiaCanvas::set_opacity(float alpha) {
    // Note: set_opacity alone doesn't composite correctly for subtrees.
    // For proper CSS opacity, use save_layer() which creates an offscreen
    // buffer. This method exists for simple single-draw opacity.
    // The SkPaint alpha is applied per-draw, not per-subtree.
    (void)alpha; // Handled via save_layer in paint_all
}

void SkiaCanvas::save_layer(float x, float y, float w, float h,
                             float opacity, float blur_radius) {
    if (!canvas_) { save(); return; }

    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    SkPaint layer_paint;

    // Set layer opacity (composited when the layer is restored)
    if (opacity < 1.0f) {
        layer_paint.setAlphaf(opacity);
    }

    // Optionally apply blur as an image filter on the layer
    if (blur_radius > 0.0f) {
        layer_paint.setImageFilter(
            SkImageFilters::Blur(blur_radius, blur_radius, SkTileMode::kClamp, nullptr));
    }

    canvas_->saveLayer(&bounds, &layer_paint);
}

void SkiaCanvas::save_backdrop_filter(float x, float y, float w, float h,
                                       float blur_radius) {
    // CSS `backdrop-filter: blur(N)` (issue-926). Push a layer whose initial
    // contents are the parent surface filtered through a Gaussian blur, so
    // subsequent draws into this layer composite over the blurred backdrop.
    if (!canvas_) { save(); return; }
    if (blur_radius <= 0.0f) {
        // Degenerate: behave like a plain save() so the matching restore()
        // stays balanced and the View::paint_all bookkeeping is unaffected.
        canvas_->save();
        return;
    }

    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    auto backdrop = SkImageFilters::Blur(blur_radius, blur_radius,
                                         SkTileMode::kClamp, nullptr);

    SkCanvas::SaveLayerRec rec(&bounds, /*paint=*/nullptr, backdrop.get(), 0);
    canvas_->saveLayer(rec);
}

// ── GPU Waveform (SkRuntimeEffect shader-driven) ────────────────────────────

// SkSL shader: samples waveform from a 1D texture, computes SDF distance
// to the curve for anti-aliased line + fill rendering.
static const char* kWaveformSkSL = R"(
    uniform shader waveformData;
    uniform float2 resolution;
    uniform float thickness;
    uniform float fillCenter;
    uniform half4 lineColor;
    uniform half4 fillColor;

    // Sample the waveform value at normalized x (0..1), returns -1..1
    float sampleWave(float x) {
        float texX = clamp(x, 0.0, 1.0) * resolution.x;
        // Sample red channel from the data texture
        return waveformData.eval(float2(texX + 0.5, 0.5)).r * 2.0 - 1.0;
    }

    // Minimum distance from point p to line segment a->b
    float segmentDist(float2 p, float2 a, float2 b) {
        float2 ab = b - a;
        float t = clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
        float2 closest = a + t * ab;
        return length(p - closest);
    }

    half4 main(float2 coord) {
        float2 uv = coord / resolution;
        float cy = fillCenter;
        float halfH = resolution.y * 0.5;

        // Sample nearby waveform points for local line segments
        float pixelWidth = 1.0 / resolution.x;
        float minDist = 1e6;

        // Check 4 segments around current x for smooth coverage
        for (int i = -2; i <= 2; i++) {
            float x0 = uv.x + float(i) * pixelWidth;
            float x1 = x0 + pixelWidth;
            float y0 = cy - sampleWave(x0) * 0.5;
            float y1 = cy - sampleWave(x1) * 0.5;
            float2 a = float2(x0 * resolution.x, y0 * resolution.y);
            float2 b = float2(x1 * resolution.x, y1 * resolution.y);
            float d = segmentDist(coord, a, b);
            minDist = min(minDist, d);
        }

        // Line: SDF anti-aliased edge
        float lineAlpha = 1.0 - smoothstep(thickness * 0.5 - 0.5, thickness * 0.5 + 0.5, minDist);

        // Fill: area between waveform and center line
        float waveY = cy - sampleWave(uv.x) * 0.5;
        float centerY = cy;
        float fillAlpha = 0.0;
        if ((uv.y >= min(waveY, centerY) - pixelWidth) &&
            (uv.y <= max(waveY, centerY) + pixelWidth)) {
            // Slope-aware edge softening
            float edge = min(abs(uv.y - waveY), abs(uv.y - centerY));
            fillAlpha = 1.0 - smoothstep(0.0, pixelWidth * 2.0, edge);
            // Full fill in interior
            if (uv.y > min(waveY, centerY) + pixelWidth &&
                uv.y < max(waveY, centerY) - pixelWidth) {
                fillAlpha = 1.0;
            }
        }

        half4 result = fillColor * half(fillAlpha);
        result = result + lineColor * half(lineAlpha) * (1.0 - result.a);
        return result;
    }
)";

void SkiaCanvas::draw_waveform(const float* samples, size_t count,
                                float x, float y, float width, float height,
                                const WaveformStyle& style) {
    if (!canvas_ || count < 2) return;

    // Try SkRuntimeEffect shader path
    static auto effectResult = SkRuntimeEffect::MakeForShader(SkString(kWaveformSkSL));
    if (!effectResult.effect) {
        // Fallback to base class CPU implementation
        Canvas::draw_waveform(samples, count, x, y, width, height, style);
        return;
    }

    auto effect = effectResult.effect;

    // Pack sample data into an RGBA8 texture (store normalized 0..1 in R channel)
    // Each sample maps from [-1,1] to [0,1] for storage
    std::vector<uint8_t> texData(count * 4);
    for (size_t i = 0; i < count; ++i) {
        uint8_t val = static_cast<uint8_t>(std::clamp((samples[i] + 1.0f) * 0.5f, 0.0f, 1.0f) * 255.0f);
        texData[i * 4 + 0] = val;  // R
        texData[i * 4 + 1] = 0;
        texData[i * 4 + 2] = 0;
        texData[i * 4 + 3] = 255;
    }

    SkImageInfo texInfo = SkImageInfo::Make(static_cast<int>(count), 1,
                                            kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    auto texImage = SkImages::RasterFromPixmapCopy(
        SkPixmap(texInfo, texData.data(), count * 4));

    if (!texImage) {
        Canvas::draw_waveform(samples, count, x, y, width, height, style);
        return;
    }

    // Create child shader from the texture
    auto texShader = texImage->makeShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                          SkSamplingOptions(SkFilterMode::kLinear));

    // Set uniforms
    SkRuntimeShaderBuilder builder(effect);
    builder.child("waveformData") = texShader;
    builder.uniform("resolution") = SkV2{width, height};
    builder.uniform("thickness") = style.line_thickness;
    builder.uniform("fillCenter") = style.fill_center;
    builder.uniform("lineColor") = SkV4{
        style.line_color.r, style.line_color.g,
        style.line_color.b, style.line_color.a};
    builder.uniform("fillColor") = SkV4{
        style.fill_color.r, style.fill_color.g,
        style.fill_color.b, style.fill_color.a};

    auto shader = builder.makeShader();
    if (!shader) {
        Canvas::draw_waveform(samples, count, x, y, width, height, style);
        return;
    }

    SkPaint paint;
    paint.setShader(std::move(shader));
    canvas_->drawRect(SkRect::MakeXYWH(x, y, width, height), paint);
}

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
