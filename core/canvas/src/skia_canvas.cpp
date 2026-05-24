#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <unordered_map>

#ifdef PULP_HAS_SKIA
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/utils/SkParsePath.h"
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
#include "include/core/SkSurface.h"
#include "include/core/SkSamplingOptions.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/effects/SkColorMatrix.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkBlendMode.h"
#include "include/effects/SkImageFilters.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#include "webgpu/webgpu_cpp.h"
#endif  // PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/emoji_segmenter.hpp>
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/font_options.hpp>
#include <pulp/canvas/text_shaper.hpp>
#ifdef PULP_HAS_SKIA
#include <pulp/canvas/text_font_context.hpp>
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TextStyle.h"
#endif

#ifdef PULP_HAS_SKIA

#include "runtime_effect_cache.hpp"
#include "skia_canvas_filter.hpp"  // parse_filter_chain — CSS `filter` parser
#include "include/effects/SkImageFilters.h"
#include "include/core/SkColorFilter.h"
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

// pulp #2163 / font v2 Slice 1.1.a — `platform_font_manager()` lives in
// bundled_fonts.cpp (exported via bundled_fonts.hpp); this TU-local shim
// stays as `get_font_manager()` for the dozens of internal call sites
// until the broader caller-migration pass moves them onto the resolver.
sk_sp<SkFontMgr> get_font_manager() {
    return platform_font_manager();
}

static SkColor to_sk_color(Color c) {
    return c.to_argb32();
}

static SkColor4f to_sk_color4f(Color c) {
    return {c.r, c.g, c.b, c.a};
}

// Solid-color fill paint. For shape fills that should honor an active
// gradient, prefer SkiaCanvas::current_fill_paint() — see #1350.
static SkPaint make_solid_fill_paint(Color c) {
    SkPaint paint;
    paint.setColor4f(to_sk_color4f(c));
    paint.setStyle(SkPaint::kFill_Style);
    paint.setAntiAlias(true);
    return paint;
}

SkPaint make_stroke_paint(Color c, float width) {
    SkPaint paint;
    paint.setColor4f(to_sk_color4f(c));
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(width);
    paint.setAntiAlias(true);
    return paint;
}

// Cached typeface loaded directly from a known path — bypasses family name
// matching for deterministic metrics (Visage-style approach). Cache key
// pulp #1737 (#932 followup) — comma-separated CSS family list walker.
// Splits a CSS `font-family` value (e.g. `"SF Pro, system-ui, sans-serif"`)
// into individual family names, stripping outer quotes and whitespace.
// Used by get_cached_typeface to walk the fallback chain — each family
// is tried in order through the existing match cascade (registered →
// bundled → SkFontMgr) until one resolves.
static std::vector<std::string> split_font_family_list(const std::string& list) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < list.size()) {
        while (i < list.size() && std::isspace(static_cast<unsigned char>(list[i]))) ++i;
        if (i >= list.size()) break;
        size_t comma = list.find(',', i);
        std::string seg = list.substr(i, (comma == std::string::npos ? list.size() : comma) - i);
        while (!seg.empty() && std::isspace(static_cast<unsigned char>(seg.back()))) seg.pop_back();
        if (seg.size() >= 2
            && (seg.front() == '"' || seg.front() == '\'')
            && seg.back() == seg.front()) {
            seg = seg.substr(1, seg.size() - 2);
        }
        if (!seg.empty()) out.push_back(std::move(seg));
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    return out;
}

// Forward decl — defined below.
static sk_sp<SkTypeface> get_cached_typeface_single(const std::string& family,
                                                    int weight, int slant);

// pulp #1737 (#932 followup) — public entry point: walks a comma-separated
// CSS family list and returns the first typeface that resolves. A single-
// family input (no comma) just delegates to get_cached_typeface_single.
// Empty string or all-fail returns the default typeface (last fallback in
// get_cached_typeface_single's chain).
static sk_sp<SkTypeface> get_cached_typeface(const std::string& family,
                                             int weight, int slant) {
    if (family.find(',') == std::string::npos) {
        return get_cached_typeface_single(family, weight, slant);
    }
    auto families = split_font_family_list(family);
    sk_sp<SkTypeface> resolved;
    for (const auto& fam : families) {
        resolved = get_cached_typeface_single(fam, weight, slant);
        if (resolved) {
            // We need to check whether the resolution is the GENUINE
            // family or the SkFontMgr's "no match → null fallback".
            // get_cached_typeface_single's last step does
            // matchFamilyStyle(nullptr) when matchFamilyStyle(family)
            // returns null — that fallback typeface has a name that
            // doesn't match `fam`. Compare and reject the fallback so
            // we keep walking the list to give later families a chance.
            SkString actual_name;
            resolved->getFamilyName(&actual_name);
            // Case-insensitive comparison — Skia normalises but some
            // platforms canonicalise differently (e.g. "Helvetica" vs
            // "Helvetica Neue"). Accept if the actual name CONTAINS
            // the requested family or vice-versa.
            std::string actual(actual_name.c_str(), actual_name.size());
            auto lower = [](std::string s) {
                for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            };
            std::string a = lower(actual), f = lower(fam);
            if (a == f || a.find(f) != std::string::npos || f.find(a) != std::string::npos) {
                return resolved;
            }
            // Else: SkFontMgr returned a default — keep walking.
        }
    }
    // No exact match across the list — return the last attempted resolution
    // (which is at least a usable fallback) or, if all attempts returned
    // null, fall through to single-family resolution of the last name to
    // exercise the standard nullptr-family fallback.
    if (resolved) return resolved;
    if (!families.empty()) return get_cached_typeface_single(families.back(), weight, slant);
    return get_cached_typeface_single("", weight, slant);
}

// includes weight + slant so setFontWeight(700) actually returns a bold
// typeface rather than the same Regular blob (pulp #927).
//
// Cache is gated on `pulp::canvas::font_registration_generation()` so that
// calling `register_font(...)` or `register_emoji_fallback(...)` mid-process
// flushes stale `SkTypeface`s — the registration header documents this
// idempotent-with-invalidation contract, but the cache never honored it
// before this fix (Codex review on emoji-parity branch).
static sk_sp<SkTypeface> get_cached_typeface_single(const std::string& family,
                                             int weight, int slant) {
    // pulp #2163 / font v2 Slice 1.1.a (caller migration) — routes through
    // FontResolver so registered / bundled / platform cascade and the
    // typeface cache live in one place. FontResolver keys on the full
    // FontOptions hash including registry_generation, so registration
    // changes invalidate automatically — replaces the
    // font_registration_generation()-watching cache that lived here.
    // The Android Roboto direct-file load is preserved as a pre-resolver
    // fast path (the resolver doesn't yet honor platform-specific file
    // overrides; restored in Slice 1.1.a finish).
#if defined(__ANDROID__)
    if (weight == 400 && slant == 0 &&
        (family == "sans-serif" || family == "Roboto" || family.empty())) {
        if (auto mgr = get_font_manager()) {
            if (auto tf = mgr->makeFromFile("/system/fonts/Roboto-Regular.ttf")) {
                return tf;
            }
        }
    }
#endif

    FontOptions opts;
    if (!family.empty()) opts.family_stack.push_back(family);
    opts.weight = static_cast<float>(weight);
    opts.slant  = slant ? FontSlant::Italic : FontSlant::Normal;

    auto resolved = FontResolver::instance().resolve_family_list(opts);
    return resolved.typeface;
}

// Legacy single-arg overload — preserves the old "Normal" behaviour for
// callers that haven't migrated to the weight/slant-aware path.
static sk_sp<SkTypeface> get_cached_typeface(const std::string& family) {
    return get_cached_typeface(family, SkFontStyle::kNormal_Weight, 0);
}

SkFont make_font(const std::string& family, float size,
                        int weight = SkFontStyle::kNormal_Weight,
                        int slant = 0,
                        bool non_opaque_dst = false) {
    SkFont font;
    // pulp #1899 — Spectr's drawSpectrum() can fire ctx.fillText calls
    // before the ctx.font setter has propagated through the JS shim
    // (race between React useEffect commit and canvas command replay).
    // If size is 0 here, fill_text would record commands but produce
    // zero-height glyphs — clamp to a minimal visible default so labels
    // render even when the JS-side font sync raced the paint.
    if (!(size > 0.0f)) size = 14.0f;
    font.setSize(size);
    font.setSubpixel(true);                               // Subpixel glyph positioning
    // pulp #1899 (gap #3) — LCD subpixel AA visibly degrades when the
    // destination surface isn't opaque: Skia's documented behavior is
    // that subpixel patterns can't antialias correctly into a partially
    // transparent pixel, so glyphs come out faint inside save_layer
    // contexts that carry alpha < 1 (e.g. an ancestor View opens
    // saveLayer(opacity = 0.6) for CSS opacity). Browsers
    // (Blink / WebKit) flip the same way — greyscale AA inside
    // non-opaque layers. Callers pass `non_opaque_dst=true` when any
    // currently-open SkiaCanvas layer was opened with alpha < 1.
    font.setEdging(non_opaque_dst
        ? SkFont::Edging::kAntiAlias            // Greyscale AA (browser parity inside opacity layers)
        : SkFont::Edging::kSubpixelAntiAlias);  // LCD-quality AA on opaque destinations
    font.setHinting(SkFontHinting::kSlight);               // Light hinting preserves glyph shapes
    font.setLinearMetrics(true);                           // Linear scaling for consistent metrics

    auto typeface = get_cached_typeface(family, weight, slant);
    if (!typeface) {
        // pulp #1899 — when no typeface resolves (empty family, missing
        // font, or pulp-screenshot's headless context missing the JS-side
        // ctx.font sync), fall back to the SkFontMgr default rather than
        // letting fill_text silently no-op. The previous behavior caused
        // all canvas text (dB axis labels, frequency labels, IIR·analog
        // sub-label) to render as nothing in Spectr's native runtime-
        // import path — bars + grid rendered fine because they don't
        // need typeface, but every fillText call silently dropped.
        auto mgr = get_font_manager();
        if (mgr) {
            SkFontStyle sk_style{
                weight, SkFontStyle::kNormal_Width,
                slant ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant
            };
            typeface = mgr->matchFamilyStyle(nullptr, sk_style);
        }
    }
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

// pulp #1737 / #1515 — restore() override applies any pending mask
// composite for the layer being restored before delegating to Skia's
// restore. Detects the matching mask layer via the save count we
// captured at save_layer_with_mask time. If no mask is pending for
// this restore (the common case — non-mask save / save_layer / etc.),
// behaves identically to the legacy `canvas_->restore()` one-liner.
//
// The mask paint pattern (see CSS Masking Module Level 1 §5):
//   1. saveLayer(content, opacity)   <- already happened at open time
//   2. ... subtree paints ...        <- already happened
//   3. saveLayer(rect, kDstIn paint)
//   4. drawRect(rect, mask shader)
//   5. restore()  <- composes mask alpha onto content (via kDstIn)
//   6. restore()  <- composes masked content to parent (kSrcOver)
//
// Steps 3-5 happen inside this override before we call canvas_->restore()
// for step 6.
void SkiaCanvas::restore() {
    GUARD_CANVAS;
    if (!pending_masks_.empty() &&
        pending_masks_.back().save_count_after_open == canvas_->getSaveCount()) {
        PendingMask m = std::move(pending_masks_.back());
        pending_masks_.pop_back();
        if (m.shader) {
            const SkRect bounds = SkRect::MakeLTRB(m.bounds.left, m.bounds.top,
                                                    m.bounds.right, m.bounds.bottom);
            // Inner kDstIn layer — its restore composes the mask
            // alpha onto the previously-painted content layer.
            SkPaint kdst_in_paint;
            kdst_in_paint.setBlendMode(SkBlendMode::kDstIn);
            canvas_->saveLayer(&bounds, &kdst_in_paint);
            SkPaint shader_paint;
            shader_paint.setShader(m.shader);
            canvas_->drawRect(bounds, shader_paint);
            canvas_->restore();  // close inner — applies mask via kDstIn
        }
        // Falls through to the outer restore() below.
    }
    // pulp #1899 (gap #3) — pop the non-opaque layer stack if the
    // layer we're about to close was tracked. Save count is captured
    // before delegating to canvas_->restore() so it matches the depth
    // recorded at push time.
    if (!non_opaque_layer_stack_.empty() &&
        non_opaque_layer_stack_.back() == canvas_->getSaveCount()) {
        non_opaque_layer_stack_.pop_back();
    }
    canvas_->restore();
}

// pulp #1368 — expose Skia's save-stack depth so CanvasWidget::paint()
// can snapshot it at entry and `restore_to_count()` back to baseline at
// exit. Without this defense an unbalanced JS draw script (e.g. a
// `ctx.save()` whose matching `ctx.restore()` is skipped on an
// early-return path) leaks GState onto the parent View's paint scope
// and silently corrupts subsequent siblings' transform/clip.
int SkiaCanvas::save_count() const {
    if (!canvas_) return 0;
    return canvas_->getSaveCount();
}

void SkiaCanvas::restore_to_count(int target) {
    GUARD_CANVAS;
    // SkCanvas::restoreToCount pops down to and including `target`; we
    // match that contract so a CanvasWidget that captured save_count()
    // == 4 at entry and got back depth 7 (three leaked saves) returns
    // to exactly 4 at exit. SkCanvas guards target == 0 internally.
    const int safe_target = target < 1 ? 1 : target;
    // pulp #1899 (gap #3) — drop any tracked non-opaque layers whose
    // save count is strictly above the target, since SkCanvas is about
    // to close all of them in one shot.
    while (!non_opaque_layer_stack_.empty() &&
           non_opaque_layer_stack_.back() > safe_target) {
        non_opaque_layer_stack_.pop_back();
    }
    canvas_->restoreToCount(safe_target);
}

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

// pulp #1368 round 2 — diagnostic CTM snapshot for `PULP_LOG_CANVAS_PAINT=1`.
// Returns the current device matrix in CanvasRenderingContext2D affine order
// so the env-gated CanvasWidget::paint logging can record what transform the
// inbound canvas actually has when paint() runs.
SkiaCanvas::AffineTransform2x3 SkiaCanvas::current_transform() const {
    AffineTransform2x3 t;
    if (!canvas_) return t;
    SkMatrix m = canvas_->getTotalMatrix();
    // SkMatrix is row-major (sx, kx, tx, ky, sy, ty, p0, p1, p2). Map to
    // CanvasRenderingContext2D column-major (a, b, c, d, e, f).
    t.a = m.getScaleX();
    t.b = m.getSkewY();
    t.c = m.getSkewX();
    t.d = m.getScaleY();
    t.e = m.getTranslateX();
    t.f = m.getTranslateY();
    return t;
}

void SkiaCanvas::clip_rect(float x, float y, float w, float h) {
    GUARD_CANVAS;
    canvas_->clipRect(SkRect::MakeXYWH(x, y, w, h));
}

void SkiaCanvas::clip(FillRule rule) {
    GUARD_CANVAS;
    if (!path_builder_) return;
    // Snapshot the path (don't detach — Canvas2D allows continued use of
    // the same path after clip()) and intersect with the current clip.
    // pulp Wave 2 cheap wiring — honour the JS-supplied fillRule arg
    // (`ctx.clip('evenodd')`) by stamping the path's fill type before
    // SkCanvas::clipPath consumes it. Default 'nonzero' matches the
    // pre-Wave-2 behaviour (SkPathFillType::kWinding).
    SkPath p = path_builder_->snapshot();
    p.setFillType(rule == FillRule::evenodd
                      ? SkPathFillType::kEvenOdd
                      : SkPathFillType::kWinding);
    canvas_->clipPath(p, /*doAntiAlias=*/true);
}

void SkiaCanvas::clip_path_svg(const std::string& svg_path_d) {
    // CSS `clip-path: path("...")` (pulp #1515). Parse the SVG-path-d
    // string via Skia's SVG path parser and install it as the canvas
    // clip. Caller owns save()/restore() balancing; we install the
    // clip on the current Skia clip stack so a paired restore() lifts
    // it. Unparseable / empty strings are silently ignored — same
    // graceful-degrade contract as the other paint-time clip helpers.
    GUARD_CANVAS;
    if (svg_path_d.empty()) return;
    SkPath path;
    if (!SkParsePath::FromSVGString(svg_path_d.c_str(), &path)) return;
    canvas_->clipPath(path, /*doAntiAlias=*/true);
}

// pulp #1350 — single source of truth for shape-fill paints. Mirrors
// `fill_current_path()` so a gradient set via `set_fill_gradient_*`
// is honored by every shape helper (rect / rrect / circle / arc /
// oval / polygon), not just path fills. The free `make_solid_fill_paint`
// remains for paths that intentionally want a solid color regardless
// of the active gradient (text glyphs today).
SkPaint SkiaCanvas::current_fill_paint() const {
    SkPaint paint;
    paint.setStyle(SkPaint::kFill_Style);
    paint.setAntiAlias(true);
    paint.setBlendMode(blend_mode_);
    if (has_gradient_ && gradient_shader_) {
        paint.setShader(gradient_shader_);
    } else {
        paint.setColor4f(to_sk_color4f(fill_color_));
    }
    apply_shadow_filter(paint);
    apply_filter(paint);
    return paint;
}

// ── Canvas2D shadow* state (issue-1434 batch 7) ──────────────────────────────
//
// Sticky values set via `ctx.shadowColor` / `shadowBlur` /
// `shadowOffsetX` / `shadowOffsetY`. Every subsequent fill/stroke draw
// queries `apply_shadow_filter` and, when active, attaches an
// `SkImageFilters::DropShadow` so the geometry emits both itself and a
// blurred shadow underneath — mirroring WebKit / Blink. Sigma mapping is
// the same `sigma = blur / 2` Chromium's Canvas2D implementation uses
// (third_party/blink/.../canvas_rendering_context_2d.cc) and matches the
// WebView reference within ~5px (same acceptance bar as #925).

bool SkiaCanvas::shadow_is_active() const {
    return shadow_color_.a > 0.0f &&
           (shadow_blur_ > 0.0f || shadow_offset_x_ != 0.0f ||
            shadow_offset_y_ != 0.0f);
}

void SkiaCanvas::apply_shadow_filter(SkPaint& paint) const {
    if (!shadow_is_active()) return;
    const float sigma = shadow_blur_ * 0.5f;
    paint.setImageFilter(SkImageFilters::DropShadow(
        shadow_offset_x_, shadow_offset_y_, sigma, sigma,
        to_sk_color4f(shadow_color_), /*colorSpace=*/nullptr,
        /*input=*/nullptr));
}

void SkiaCanvas::set_shadow_color(Color color) { shadow_color_ = color; }
void SkiaCanvas::set_shadow_blur(float blur) {
    shadow_blur_ = std::max(0.0f, blur);  // Spec: negative blur is invalid → ignored
}
void SkiaCanvas::set_shadow_offset_x(float dx) { shadow_offset_x_ = dx; }
void SkiaCanvas::set_shadow_offset_y(float dy) { shadow_offset_y_ = dy; }

void SkiaCanvas::set_fill_color(Color c) { fill_color_ = c; }
void SkiaCanvas::set_stroke_color(Color c) { stroke_color_ = c; }
void SkiaCanvas::set_line_width(float w) { line_width_ = w; }

void SkiaCanvas::set_line_cap(LineCap cap) {
    line_cap_ = cap;
}

void SkiaCanvas::set_line_join(LineJoin join) {
    line_join_ = join;
}

// pulp #1434 bridge-thin gap-fill — wire ctx.miterLimit through to
// SkPaint::setStrokeMiter. The current stroke helpers build a fresh
// SkPaint per draw via make_stroke_paint(); we apply the miter limit
// at draw-time in stroke_current_path / stroke_rect / etc., so this
// setter just stores the value and lets the apply_* helper read it.
// Spec: non-positive / non-finite values are silently ignored.
void SkiaCanvas::set_miter_limit(float limit) {
    if (std::isfinite(limit) && limit > 0.0f) {
        miter_limit_ = limit;
    }
}

// pulp #1434 bridge-thin gap-fill — wire ctx.imageSmoothingEnabled and
// ctx.imageSmoothingQuality through. The flag is read at drawImage
// time by sampling_options_for_image_smoothing() below; we just store
// the chosen state here so it sticks across draws.
void SkiaCanvas::set_image_smoothing(bool enabled,
                                     ImageSmoothingQuality quality) {
    image_smoothing_enabled_ = enabled;
    image_smoothing_quality_ = quality;
}

// ── pulp #1520 — Canvas2D ctx.direction / ctx.filter ─────────────────────
// Direction is stored on the canvas state and read at fill_text() time
// to choose the SkShaper leftToRight flag. RTL flips the flag, which is
// the correct first step for proper Arabic / Hebrew shaping; full bidi
// (mixed-script paragraphs requiring the Unicode Bidi Algorithm) needs
// SkParagraph plumbing tracked under #1506. inherit currently behaves
// as ltr until per-View writing-direction lookup lands.
void SkiaCanvas::set_direction(TextDirection direction) {
    direction_ = direction;
}

// The CSS `filter` / `ctx.filter` parsing cluster (skip_ws,
// parse_filter_arg{,s,_length}, parse_css_color_to_skcolor, the
// SkColorMatrix builders, parse_filter_chain) was extracted into the
// sibling TU skia_canvas_filter.cpp so filter-parsing work no longer
// recompiles this file. `parse_filter_chain` is the only cross-TU
// entry point — declared in skia_canvas_filter.hpp below.

void SkiaCanvas::set_filter(const std::string& filter) {
    filter_source_ = filter;
    filter_image_filter_ = parse_filter_chain(filter);
}

void SkiaCanvas::apply_filter(SkPaint& paint) const {
    if (!filter_image_filter_) return;
    // If a shadow filter is already set on this paint, compose the user
    // filter on top so the chain reads outer = filter(inner = shadow).
    if (auto existing = paint.refImageFilter()) {
        paint.setImageFilter(SkImageFilters::Compose(
            filter_image_filter_, std::move(existing)));
    } else {
        paint.setImageFilter(filter_image_filter_);
    }
}

// pulp #1434 bridge-thin gap-fill — apply sticky stroke-paint state. The
// existing stroke paths constructed an SkPaint via make_stroke_paint()
// but never propagated the JS-side line_join / miter_limit state. The
// canvas2d harness flagged miterLimit as silently dropped; setStrokeJoin
// and setStrokeMiter close that gap without touching every call site.
// line_cap_ is plumbed here too so the existing line_cap_ field finally
// reaches the GPU paint — matches Canvas2D ctx.lineCap semantics.
void SkiaCanvas::apply_stroke_state(SkPaint& paint) const {
    SkPaint::Cap sk_cap = SkPaint::kButt_Cap;
    switch (line_cap_) {
        case LineCap::butt:   sk_cap = SkPaint::kButt_Cap;   break;
        case LineCap::round:  sk_cap = SkPaint::kRound_Cap;  break;
        case LineCap::square: sk_cap = SkPaint::kSquare_Cap; break;
    }
    paint.setStrokeCap(sk_cap);

    SkPaint::Join sk_join = SkPaint::kMiter_Join;
    switch (line_join_) {
        case LineJoin::miter: sk_join = SkPaint::kMiter_Join; break;
        case LineJoin::round: sk_join = SkPaint::kRound_Join; break;
        case LineJoin::bevel: sk_join = SkPaint::kBevel_Join; break;
    }
    paint.setStrokeJoin(sk_join);
    paint.setStrokeMiter(miter_limit_);
    // pulp #1434 bridge-thin gap-fill — Canvas2D ctx.createPattern on
    // strokeStyle. Wins over the solid stroke colour when present.
    if (stroke_shader_) {
        paint.setShader(stroke_shader_);
    }
}

// pulp #1434 bridge-thin gap-fill — translate Canvas2D
// imageSmoothingEnabled / imageSmoothingQuality into Skia sampling.
// `enabled = false` => nearest (pixel-art preservation). `enabled = true`
// honours the quality enum: low = bilinear (matches the existing default),
// medium = mipmap-aware bilinear, high = cubic Mitchell. Falls through
// to kLinear so callers that haven't touched the flag get unchanged
// behavior.
SkSamplingOptions SkiaCanvas::sampling_options_for_image_smoothing() const {
    if (!image_smoothing_enabled_) {
        return SkSamplingOptions(SkFilterMode::kNearest);
    }
    switch (image_smoothing_quality_) {
        case ImageSmoothingQuality::low:
            return SkSamplingOptions(SkFilterMode::kLinear);
        case ImageSmoothingQuality::medium:
            return SkSamplingOptions(SkFilterMode::kLinear,
                                     SkMipmapMode::kLinear);
        case ImageSmoothingQuality::high:
            return SkSamplingOptions(SkCubicResampler::Mitchell());
    }
    return SkSamplingOptions(SkFilterMode::kLinear);
}

void SkiaCanvas::fill_rect(float x, float y, float w, float h) {
    GUARD_CANVAS; canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), current_fill_paint());
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
    apply_stroke_state(paint);
    apply_line_dash(paint, line_dash_, line_dash_phase_);
    apply_shadow_filter(paint);
    apply_filter(paint);
    canvas_->drawRect(SkRect::MakeXYWH(x, y, w, h), paint);
}

void SkiaCanvas::fill_rounded_rect(float x, float y, float w, float h, float radius) {
    GUARD_CANVAS; SkRRect rrect;
    rrect.setRectXY(SkRect::MakeXYWH(x, y, w, h), radius, radius);
    canvas_->drawRRect(rrect, current_fill_paint());
}

void SkiaCanvas::stroke_rounded_rect(float x, float y, float w, float h, float radius) {
    GUARD_CANVAS; SkRRect rrect;
    rrect.setRectXY(SkRect::MakeXYWH(x, y, w, h), radius, radius);
    auto paint = make_stroke_paint(stroke_color_, line_width_);
    apply_stroke_state(paint);
    apply_line_dash(paint, line_dash_, line_dash_phase_);
    apply_shadow_filter(paint);
    apply_filter(paint);
    canvas_->drawRRect(rrect, paint);
}

void SkiaCanvas::fill_circle(float cx, float cy, float radius) {
    GUARD_CANVAS; canvas_->drawCircle(cx, cy, radius, current_fill_paint());
}

void SkiaCanvas::stroke_circle(float cx, float cy, float radius) {
    GUARD_CANVAS;
    auto paint = make_stroke_paint(stroke_color_, line_width_);
    apply_stroke_state(paint);
    apply_line_dash(paint, line_dash_, line_dash_phase_);
    apply_shadow_filter(paint);
    apply_filter(paint);
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
        apply_stroke_state(paint);
        apply_line_dash(paint, line_dash_, line_dash_phase_);
        apply_shadow_filter(paint);
        apply_filter(paint);
        canvas_->drawPath(path, paint);
    }
}

void SkiaCanvas::stroke_line(float x0, float y0, float x1, float y1) {
    GUARD_CANVAS;
    auto paint = make_stroke_paint(stroke_color_, line_width_);
    apply_stroke_state(paint);
    apply_line_dash(paint, line_dash_, line_dash_phase_);
    apply_shadow_filter(paint);
    apply_filter(paint);
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

// pulp #1737 — capture OpenType feature flags (e.g. tnum / smcp) for
// SkShaper's 8-arg shape() overload. Empty vector clears the active
// features. Per-feature semantics:
//   value = 0 → disable the feature
//   value = 1 → enable the feature
//   value > 1 → feature-specific (e.g. ss01..ss20 for stylistic sets)
void SkiaCanvas::set_font_features(std::vector<FontFeature> features) {
    font_features_ = std::move(features);
}
void SkiaCanvas::clear_font_features() {
    font_features_.clear();
}

// ── Images ──────────────────────────────────────────────────────────────────

bool SkiaCanvas::draw_image_from_data(const uint8_t* data, size_t size,
                                       float x, float y, float w, float h) {
    if (!canvas_ || !data || size == 0) return false;

    auto sk_data = SkData::MakeWithoutCopy(data, size);
    auto image = SkImages::DeferredFromEncodedData(sk_data);
    if (!image) return false;

    // pulp #1434 — honour the sticky imageSmoothingEnabled / Quality state.
    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h),
                           sampling_options_for_image_smoothing());
    return true;
}

bool SkiaCanvas::draw_image_from_file(const std::string& path,
                                       float x, float y, float w, float h) {
    if (!canvas_ || path.empty()) return false;

    auto sk_data = SkData::MakeFromFileName(path.c_str());
    if (!sk_data) return false;

    auto image = SkImages::DeferredFromEncodedData(sk_data);
    if (!image) return false;

    // pulp #1434 — honour the sticky imageSmoothingEnabled / Quality state.
    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h),
                           sampling_options_for_image_smoothing());
    return true;
}

bool SkiaCanvas::draw_skia_image(const sk_sp<SkImage>& image,
                                  float x, float y, float w, float h) {
    if (!canvas_ || !image) return false;
    canvas_->drawImageRect(image, SkRect::MakeXYWH(x, y, w, h),
                           sampling_options_for_image_smoothing());
    return true;
}

// pulp #1737 — 9-arg drawImage source-rect form. Skia's drawImageRect has
// a four-rect overload that maps `src` (in image coords) onto `dst` (in
// canvas coords) with the active sampling. Used by sprite-sheet slicing.
bool SkiaCanvas::draw_image_from_data_rect(const uint8_t* data, size_t size,
                                            float sx, float sy, float sw, float sh,
                                            float dx, float dy, float dw, float dh) {
    if (!canvas_ || !data || size == 0) return false;
    auto sk_data = SkData::MakeWithoutCopy(data, size);
    auto image = SkImages::DeferredFromEncodedData(sk_data);
    if (!image) return false;
    canvas_->drawImageRect(image,
                           SkRect::MakeXYWH(sx, sy, sw, sh),
                           SkRect::MakeXYWH(dx, dy, dw, dh),
                           sampling_options_for_image_smoothing(),
                           nullptr,
                           SkCanvas::kStrict_SrcRectConstraint);
    return true;
}

bool SkiaCanvas::draw_image_from_file_rect(const std::string& path,
                                            float sx, float sy, float sw, float sh,
                                            float dx, float dy, float dw, float dh) {
    if (!canvas_ || path.empty()) return false;
    auto sk_data = SkData::MakeFromFileName(path.c_str());
    if (!sk_data) return false;
    auto image = SkImages::DeferredFromEncodedData(sk_data);
    if (!image) return false;
    canvas_->drawImageRect(image,
                           SkRect::MakeXYWH(sx, sy, sw, sh),
                           SkRect::MakeXYWH(dx, dy, dw, dh),
                           sampling_options_for_image_smoothing(),
                           nullptr,
                           SkCanvas::kStrict_SrcRectConstraint);
    return true;
}

// pulp #1737 — peek-only image dimensions for ImageView object-fit /
// object-position layout. Decode is deferred until first paint, so this
// just builds the SkImage header to read width()/height() — Skia caches
// the SkImageInfo so the next draw_image_from_file in the same paint
// pass reuses it (the GPU decode lives in the SkImages::DeferredFrom...
// shared chain).
bool SkiaCanvas::measure_image_from_file(const std::string& path,
                                          float& out_width, float& out_height) {
    out_width = 0.0f; out_height = 0.0f;
    if (path.empty()) return false;
    auto sk_data = SkData::MakeFromFileName(path.c_str());
    if (!sk_data) return false;
    auto image = SkImages::DeferredFromEncodedData(sk_data);
    if (!image) return false;
    out_width  = static_cast<float>(image->width());
    out_height = static_cast<float>(image->height());
    return true;
}

// ── Blend modes ─────────────────────────────────────────────────────────────

// pulp #1549 — shared canvas BlendMode -> SkBlendMode lookup. Used by
// set_blend_mode() and save_layer_with_blend() so the two paths can never
// drift on the keyword set.
static SkBlendMode skia_blend_mode_for(Canvas::BlendMode mode) {
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
    if (idx < 0 || idx >= count) return SkBlendMode::kSrcOver;
    return map[idx];
}

void SkiaCanvas::set_blend_mode(BlendMode mode) {
    // Indices 0..15 — advanced/W3C blend modes (must stay in sync with
    // canvas.hpp BlendMode enum).
    // Indices 16..26 — Porter-Duff compositing modes (issue-896).
    blend_mode_ = skia_blend_mode_for(mode);
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

// ── pulp #1521 — native arc / arcTo / ellipse / roundRect path builders ──
//
// Replaces the JS shim's bezier approximation with Skia's native arc APIs.
// Skia ships a closed-form arc-to-cubic implementation behind SkPath::arcTo;
// we use the (oval, startDeg, sweepDeg, forceMoveTo=false) overload for
// `arc` / `ellipse` and the (p1, p2, radius) overload for `arcTo`.
// Per-corner roundRect uses SkRRect::MakeRectRadii.
//
// Angles arrive as radians (Canvas2D spec). Skia's arcTo wants degrees.
// `anticlockwise` flips the sweep sign per the HTML5 spec: when false, the
// arc is drawn clockwise from start to end; when true, anticlockwise. The
// shim's normalization (already in JS) ensured `sweep` was in the right
// direction; we re-normalize here so direct C++ callers (e.g. tests, AOT
// paths) get the same behavior without going through JS.
namespace {
constexpr float kRadToDeg = 57.29577951308232f; // 180 / PI
constexpr float kTwoPi = 6.283185307179586f;

// Normalize (start, end, anticlockwise) into a (startDeg, sweepDeg) pair
// matching Canvas2D semantics. Mirrors the JS shim:
//   sweep = end - start
//   if anticlockwise && sweep > 0  -> sweep -= 2π
//   if !anticlockwise && sweep < 0 -> sweep += 2π
// The full-circle case (|sweep| >= 2π) is clamped to 2π so SkPath::arcTo
// produces a complete circle rather than wrapping multiple times.
void normalize_arc_sweep(float start_rad, float end_rad,
                         bool anticlockwise,
                         float& start_deg_out, float& sweep_deg_out) {
    float sweep_rad = end_rad - start_rad;
    if (anticlockwise) {
        if (sweep_rad > 0) sweep_rad -= kTwoPi;
    } else {
        if (sweep_rad < 0) sweep_rad += kTwoPi;
    }
    if (sweep_rad > kTwoPi) sweep_rad = kTwoPi;
    if (sweep_rad < -kTwoPi) sweep_rad = -kTwoPi;
    start_deg_out = start_rad * kRadToDeg;
    sweep_deg_out = sweep_rad * kRadToDeg;
}
} // namespace

void SkiaCanvas::arc(float cx, float cy, float radius,
                     float start_angle, float end_angle,
                     bool anticlockwise) {
    if (!path_builder_) return;
    if (radius <= 0.0f) return;
    SkRect oval = SkRect::MakeLTRB(cx - radius, cy - radius,
                                    cx + radius, cy + radius);
    float start_deg = 0.0f, sweep_deg = 0.0f;
    normalize_arc_sweep(start_angle, end_angle, anticlockwise,
                        start_deg, sweep_deg);
    // forceMoveTo=false so the arc connects to the current point with an
    // implicit lineTo, matching the Canvas2D spec which says arc() adds
    // an arc to the current subpath.
    path_builder_->arcTo(oval, start_deg, sweep_deg, /*forceMoveTo=*/false);
}

void SkiaCanvas::arc_to(float x1, float y1, float x2, float y2, float radius) {
    if (!path_builder_) return;
    if (radius <= 0.0f) {
        // Spec: a non-positive radius collapses to a lineTo to (x1, y1).
        path_builder_->lineTo(x1, y1);
        return;
    }
    // The 5-arg SkPath::arcTo overload computes the tangent arc between
    // the current point, (x1, y1), and (x2, y2). It internally handles
    // the degenerate collinear case by emitting a lineTo to (x1, y1).
    path_builder_->arcTo(SkPoint{x1, y1}, SkPoint{x2, y2}, radius);
}

void SkiaCanvas::ellipse(float cx, float cy, float rx, float ry,
                         float rotation,
                         float start_angle, float end_angle,
                         bool anticlockwise) {
    if (!path_builder_) return;
    if (rx <= 0.0f || ry <= 0.0f) return;
    float start_deg = 0.0f, sweep_deg = 0.0f;
    normalize_arc_sweep(start_angle, end_angle, anticlockwise,
                        start_deg, sweep_deg);
    if (rotation == 0.0f) {
        SkRect oval = SkRect::MakeLTRB(cx - rx, cy - ry, cx + rx, cy + ry);
        path_builder_->arcTo(oval, start_deg, sweep_deg, /*forceMoveTo=*/false);
        return;
    }
    // Rotated ellipse: build the arc into a temporary oval-aligned path,
    // rotate it through SkMatrix around (cx, cy), and append. This keeps
    // the arc geometry exact (still a real arc, not a bezier approx) but
    // honours the CSS rotation parameter.
    SkPathBuilder tmp;
    SkRect oval = SkRect::MakeLTRB(-rx, -ry, rx, ry);
    tmp.arcTo(oval, start_deg, sweep_deg, /*forceMoveTo=*/true);
    SkMatrix m = SkMatrix::I();
    m.preTranslate(cx, cy);
    m.preRotate(rotation * kRadToDeg);
    // Skia m144 (chrome): SkPath::transform(SkMatrix) is gone — apply
    // transform on the builder before detaching, instead of mutating
    // the SkPath after. Same semantics, m144-compatible API.
    tmp.transform(m);
    SkPath rotated = tmp.detach();
    // Append rotated path into the live builder, preserving the current
    // subpath. kExtend connects the rotated arc's first point to the
    // existing pen position with an implicit lineTo (matching CSS
    // Canvas2D semantics for ellipse: "If the current point is not at
    // the start of the arc, a straight line is added."). The earlier
    // kAppend default replaced that with a moveTo, breaking contour
    // continuity for fills (Codex #1616 P1 on #1556). When the builder
    // is empty kExtend degrades to kAppend, so unrotated standalone
    // ellipses still work.
    path_builder_->addPath(rotated, SkPath::kExtend_AddPathMode);
}

void SkiaCanvas::round_rect(float x, float y, float w, float h,
                            float tl_x, float tl_y,
                            float tr_x, float tr_y,
                            float br_x, float br_y,
                            float bl_x, float bl_y) {
    if (!path_builder_) return;
    if (w <= 0.0f || h <= 0.0f) return;
    SkRect rect = SkRect::MakeXYWH(x, y, w, h);
    SkVector radii[4] = {
        {tl_x, tl_y}, {tr_x, tr_y}, {br_x, br_y}, {bl_x, bl_y}
    };
    SkRRect rr;
    rr.setRectRadii(rect, radii);
    path_builder_->addRRect(rr);
}

void SkiaCanvas::fill_current_path(FillRule rule) {
    if (!canvas_ || !path_builder_) return;
    SkPaint paint;
    paint.setAntiAlias(true);
    if (has_gradient_ && gradient_shader_) {
        paint.setShader(gradient_shader_);
    } else {
        paint.setColor4f(to_sk_color4f(fill_color_));
    }
    paint.setBlendMode(blend_mode_);
    apply_shadow_filter(paint);
    apply_filter(paint);
    // pulp #1806 — snapshot, not detach. Canvas2D spec: `ctx.fill()` and
    // `ctx.stroke()` paint the scratch path WITHOUT consuming it. A
    // subsequent `stroke()` on the same path must produce the outlined
    // version of the filled shape. `detach()` was leaving the builder
    // empty, so any caller doing `fill → stroke` (SvgPathWidget compound
    // paths, common JS canvas idiom) silently dropped the second op.
    // Stamp the JS-supplied fillRule (`ctx.fill('evenodd')`) onto the
    // snapshot so Skia honours it when computing the filled area;
    // default 'nonzero' keeps the SkPathFillType::kWinding behaviour.
    SkPath p = path_builder_->snapshot();
    p.setFillType(rule == FillRule::evenodd
                      ? SkPathFillType::kEvenOdd
                      : SkPathFillType::kWinding);
    canvas_->drawPath(p, paint);
}

void SkiaCanvas::stroke_current_path() {
    if (!canvas_ || !path_builder_) return;
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setColor4f(to_sk_color4f(stroke_color_));
    paint.setStrokeWidth(line_width_);
    paint.setBlendMode(blend_mode_);
    apply_stroke_state(paint);
    apply_line_dash(paint, line_dash_, line_dash_phase_);
    apply_shadow_filter(paint);
    apply_filter(paint);
    // pulp #1806 — snapshot (preserve), not detach. See fill_current_path.
    canvas_->drawPath(path_builder_->snapshot(), paint);
}

// ── Static SkSL compilation (accessible from bridge without Canvas instance) ──

std::string Canvas::compile_sksl(const std::string& sksl) {
    if (sksl.empty()) return "Empty shader code";
    auto& cache = RuntimeEffectCache::instance();
    auto effect = cache.get_or_compile(sksl);
    return effect ? "" : cache.last_error();
}


} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
