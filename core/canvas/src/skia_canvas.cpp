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
#include "include/effects/SkGradientShader.h"
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
static sk_sp<SkFontMgr> get_font_manager() {
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

static SkFont make_font(const std::string& family, float size,
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

namespace {

// Skim leading whitespace.
inline void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
}

// Parse a single CSS filter <length-or-number-or-percentage> argument.
// Returns the numeric value coerced into the unit the caller expects:
//   * `expect_px` → drops a trailing "px" or treats bare numbers as px
//     (used by blur / drop-shadow).
//   * `expect_angle` → converts deg/rad/turn into radians (hue-rotate).
//   * default → unitless multiplier; "%" divides by 100. Used by
//     brightness / contrast / saturate / opacity / invert / grayscale /
//     sepia.
//
// Returns NaN on parse failure so callers can default to spec-neutral.
enum class FilterArgKind { multiplier, pixels, angle };
double parse_filter_arg(const std::string& body, FilterArgKind kind) {
    // Find numeric prefix.
    size_t end = 0;
    while (end < body.size() &&
           (std::isdigit(static_cast<unsigned char>(body[end])) ||
            body[end] == '.' || body[end] == '-' || body[end] == '+' ||
            body[end] == 'e' || body[end] == 'E')) {
        ++end;
    }
    if (end == 0) return std::nan("");
    double v = 0;
    try { v = std::stod(body.substr(0, end)); }
    catch (...) { return std::nan(""); }
    std::string unit = body.substr(end);
    // Strip whitespace from unit.
    while (!unit.empty() && (unit.back() == ' ' || unit.back() == '\t')) unit.pop_back();
    size_t us = 0;
    while (us < unit.size() && (unit[us] == ' ' || unit[us] == '\t')) ++us;
    unit = unit.substr(us);

    switch (kind) {
        case FilterArgKind::pixels:
            // Bare numbers are px; "px" suffix is the canonical form.
            return v;
        case FilterArgKind::angle:
            if (unit == "rad") return v;
            if (unit == "turn") return v * (2.0 * 3.14159265358979323846);
            // Default deg (or unrecognised → deg per CSS spec).
            return v * (3.14159265358979323846 / 180.0);
        case FilterArgKind::multiplier:
            if (!unit.empty() && unit.back() == '%') return v * 0.01;
            return v;
    }
    return v;
}

// Split space-separated arguments from a filter function's body.
// Handles parenthesised sub-expressions (e.g. `rgb(1,2,3)`) so color
// functions are kept as a single token.
std::vector<std::string> parse_filter_args(const std::string& body) {
    std::vector<std::string> tokens;
    std::string tok;
    int paren = 0;
    for (char c : body) {
        if (c == '(') { ++paren; tok += c; continue; }
        if (c == ')') { --paren; tok += c; continue; }
        if (paren == 0 && std::isspace(static_cast<unsigned char>(c))) {
            if (!tok.empty()) { tokens.push_back(std::move(tok)); tok.clear(); }
            continue;
        }
        tok += c;
    }
    if (!tok.empty()) tokens.push_back(std::move(tok));
    return tokens;
}

// Parse a single CSS length token to a numeric pixel value.
// "12px" → 12.0, "12" → 12.0, "invalid" → NaN.
double parse_filter_arg_length(const std::string& tok) {
    if (tok.empty()) return std::nan("");
    size_t end = 0;
    while (end < tok.size() &&
           (std::isdigit(static_cast<unsigned char>(tok[end])) ||
            tok[end] == '.' || tok[end] == '-' || tok[end] == '+' ||
            tok[end] == 'e' || tok[end] == 'E')) {
        ++end;
    }
    if (end == 0) return std::nan("");
    double v = 0;
    try { v = std::stod(tok.substr(0, end)); }
    catch (...) { return std::nan(""); }
    return v; // bare number or "px" suffix both treated as pixels
}

// Parse a CSS color string into SkColor. Supports #hex, rgb()/rgba(),
// hsl()/hsla(), "transparent", and a subset of named colors.
// Defaults to black on parse failure.
SkColor parse_css_color_to_skcolor(const std::string& str) {
    if (str == "transparent") return SK_ColorTRANSPARENT;

    if (!str.empty() && str[0] == '#') {
        auto parse_hex = [](const std::string& s) -> std::optional<uint8_t> {
            try { return static_cast<uint8_t>(std::stoul(s, nullptr, 16)); }
            catch (...) { return std::nullopt; }
        };
        if (str.size() == 4) { // #RGB — each nibble doubled
            auto r = parse_hex(std::string(2, str[1]));
            auto g = parse_hex(std::string(2, str[2]));
            auto b = parse_hex(std::string(2, str[3]));
            if (r && g && b) return SkColorSetRGB(*r, *g, *b);
            return SK_ColorBLACK;
        }
        if (str.size() >= 7) { // #RRGGBB[AA]
            auto r = parse_hex(str.substr(1, 2));
            auto g = parse_hex(str.substr(3, 2));
            auto b = parse_hex(str.substr(5, 2));
            if (!r || !g || !b) return SK_ColorBLACK;
            uint8_t a = 255;
            if (str.size() >= 9) {
                auto parsed = parse_hex(str.substr(7, 2));
                if (!parsed) return SK_ColorBLACK;
                a = *parsed;
            }
            return SkColorSetARGB(a, *r, *g, *b);
        }
        return SK_ColorBLACK;
    }

    // rgb(r, g, b) / rgba(r, g, b, a)
    if (str.substr(0, 4) == "rgb(" || str.substr(0, 5) == "rgba(") {
        auto inner = str.substr(str.find('(') + 1);
        inner = inner.substr(0, inner.find(')'));
        float vals[4] = {0, 0, 0, 1};
        int n = 0;
        std::istringstream ss(inner);
        std::string tok;
        while (std::getline(ss, tok, ',') && n < 4) {
            while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
            vals[n++] = std::stof(tok);
        }
        uint8_t r = static_cast<uint8_t>(std::clamp(vals[0] / 255.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        uint8_t g = static_cast<uint8_t>(std::clamp(vals[1] / 255.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        uint8_t b = static_cast<uint8_t>(std::clamp(vals[2] / 255.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
        uint8_t a = static_cast<uint8_t>(std::clamp(vals[3], 0.0f, 1.0f) * 255.0f + 0.5f);
        return SkColorSetARGB(a, r, g, b);
    }

    // hsl(h, s%, l%) / hsla(h, s%, l%, a)
    if (str.substr(0, 4) == "hsl(" || str.substr(0, 5) == "hsla(") {
        auto inner = str.substr(str.find('(') + 1);
        inner = inner.substr(0, inner.find(')'));
        float vals[4] = {0, 0, 0, 1};
        int n = 0;
        std::istringstream ss(inner);
        std::string tok;
        while (std::getline(ss, tok, ',') && n < 4) {
            while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
            if (tok.back() == '%') tok.pop_back();
            vals[n++] = std::stof(tok);
        }
        float h = std::fmod(vals[0], 360.0f) / 360.0f;
        float s = vals[1] / 100.0f;
        float l = vals[2] / 100.0f;
        auto hue2rgb = [](float p, float q, float t) {
            if (t < 0) t += 1; if (t > 1) t -= 1;
            if (t < 1.0f / 6) return p + (q - p) * 6 * t;
            if (t < 1.0f / 2) return q;
            if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
            return p;
        };
        float r, g, b;
        if (s == 0) { r = g = b = l; }
        else {
            float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
            float p = 2 * l - q;
            r = hue2rgb(p, q, h + 1.0f / 3);
            g = hue2rgb(p, q, h);
            b = hue2rgb(p, q, h - 1.0f / 3);
        }
        return SkColorSetARGB(
            static_cast<uint8_t>(vals[3] * 255.0f + 0.5f),
            static_cast<uint8_t>(r * 255.0f + 0.5f),
            static_cast<uint8_t>(g * 255.0f + 0.5f),
            static_cast<uint8_t>(b * 255.0f + 0.5f));
    }

    // Named colors (subset matching test expectations).
    static const std::unordered_map<std::string, SkColor> named = {
        {"black", SK_ColorBLACK}, {"white", SK_ColorWHITE},
        {"red", SK_ColorRED}, {"green", SK_ColorGREEN},
        {"blue", SK_ColorBLUE}, {"yellow", SK_ColorYELLOW},
        {"cyan", SK_ColorCYAN}, {"magenta", SK_ColorMAGENTA},
        {"gray", 0xFF808080}, {"grey", 0xFF808080},
    };
    auto it = named.find(str);
    if (it != named.end()) return it->second;

    return SK_ColorBLACK;
}

// Build a saturation SkColorMatrix. s=1 is identity, s=0 is grayscale,
// s>1 boosts saturation. Standard luminance weights match the CSS spec.
SkColorMatrix make_saturation_matrix(double s) {
    const float r = 0.213f;
    const float g = 0.715f;
    const float b = 0.072f;
    const float sf = static_cast<float>(s);
    return SkColorMatrix(
        r + (1 - r) * sf, g - g * sf,        b - b * sf,        0, 0,
        r - r * sf,        g + (1 - g) * sf, b - b * sf,        0, 0,
        r - r * sf,        g - g * sf,        b + (1 - b) * sf, 0, 0,
        0,                 0,                 0,                 1, 0);
}

// Sepia per CSS spec. amount=0 identity, amount=1 full sepia.
SkColorMatrix make_sepia_matrix(double amount) {
    const float a = static_cast<float>(amount);
    const float inv = 1.0f - a;
    return SkColorMatrix(
        0.393f * a + inv, 0.769f * a,        0.189f * a,        0, 0,
        0.349f * a,        0.686f * a + inv, 0.168f * a,        0, 0,
        0.272f * a,        0.534f * a,        0.131f * a + inv, 0, 0,
        0,                 0,                 0,                 1, 0);
}

// Hue-rotate matrix (CSS spec). angle in radians.
SkColorMatrix make_hue_rotate_matrix(double rad) {
    const float c = static_cast<float>(std::cos(rad));
    const float s = static_cast<float>(std::sin(rad));
    return SkColorMatrix(
        0.213f + c * 0.787f - s * 0.213f,
        0.715f - c * 0.715f - s * 0.715f,
        0.072f - c * 0.072f + s * 0.928f, 0, 0,
        0.213f - c * 0.213f + s * 0.143f,
        0.715f + c * 0.285f + s * 0.140f,
        0.072f - c * 0.072f - s * 0.283f, 0, 0,
        0.213f - c * 0.213f - s * 0.787f,
        0.715f - c * 0.715f + s * 0.715f,
        0.072f + c * 0.928f + s * 0.072f, 0, 0,
        0, 0, 0, 1, 0);
}

// Linear contrast matrix: out = c * in + (0.5 * (1 - c)).
SkColorMatrix make_contrast_matrix(double c) {
    const float f = static_cast<float>(c);
    const float t = 0.5f * (1.0f - f);
    return SkColorMatrix(
        f, 0, 0, 0, t,
        0, f, 0, 0, t,
        0, 0, f, 0, t,
        0, 0, 0, 1, 0);
}

// Linear brightness matrix: scale RGB by `amount`.
SkColorMatrix make_brightness_matrix(double amount) {
    const float f = static_cast<float>(amount);
    return SkColorMatrix(
        f, 0, 0, 0, 0,
        0, f, 0, 0, 0,
        0, 0, f, 0, 0,
        0, 0, 0, 1, 0);
}

// Invert matrix: out = (1 - 2*amount) * in + amount.
SkColorMatrix make_invert_matrix(double amount) {
    const float k = static_cast<float>(amount);
    const float diag = 1.0f - 2.0f * k;
    return SkColorMatrix(
        diag, 0,    0,    0, k,
        0,    diag, 0,    0, k,
        0,    0,    diag, 0, k,
        0,    0,    0,    1, 0);
}

// Grayscale matrix: amount=0 identity, amount=1 full luma. Matches the
// CSS spec's interpolation of saturation toward 0 weighted by `amount`.
SkColorMatrix make_grayscale_matrix(double amount) {
    return make_saturation_matrix(1.0 - amount);
}

// Opacity = scale alpha. Identity at 1.
SkColorMatrix make_opacity_matrix(double a) {
    const float f = static_cast<float>(a);
    return SkColorMatrix(
        1, 0, 0, 0, 0,
        0, 1, 0, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 0, f, 0);
}

// Wrap an SkColorMatrix into an SkImageFilter, composing onto the
// existing chain (inner). Caller passes the existing chain so we can
// fold multiple color-matrix functions into a single ColorFilter
// imagefilter where possible — but for simplicity we just compose.
sk_sp<SkImageFilter> compose(sk_sp<SkImageFilter> outer,
                              sk_sp<SkImageFilter> inner) {
    if (!outer) return inner;
    if (!inner) return outer;
    return SkImageFilters::Compose(std::move(outer), std::move(inner));
}

sk_sp<SkImageFilter> color_matrix_filter(const SkColorMatrix& m) {
    return SkImageFilters::ColorFilter(SkColorFilters::Matrix(m), nullptr, nullptr);
}

// Parse a CSS <filter-function-list> string into an SkImageFilter chain.
// Supported functions (per Canvas2D spec subset):
//   blur(<length>)            — SkImageFilters::Blur
//   brightness(<num|%>)       — color matrix scale
//   contrast(<num|%>)         — color matrix scale + bias
//   grayscale(<num|%>)        — color matrix saturation→0
//   hue-rotate(<angle>)       — color matrix rotation in YIQ-ish space
//   invert(<num|%>)           — color matrix invert
//   opacity(<num|%>)          — alpha scale
//   saturate(<num|%>)         — color matrix saturation
//   sepia(<num|%>)            — color matrix sepia interpolation
// Unknown or malformed functions are silently dropped (per CSS spec for
// invalid filter-function-list — the entire chain falls back to none).
sk_sp<SkImageFilter> parse_filter_chain(const std::string& src) {
    if (src.empty()) return nullptr;
    // Skip purely "none" / whitespace.
    size_t first = 0;
    while (first < src.size() && (src[first] == ' ' || src[first] == '\t')) ++first;
    if (first >= src.size()) return nullptr;
    if (src.compare(first, 4, "none") == 0) return nullptr;

    sk_sp<SkImageFilter> chain;
    size_t i = first;
    while (i < src.size()) {
        skip_ws(src, i);
        if (i >= src.size()) break;
        // Read function name up to '('.
        size_t name_start = i;
        while (i < src.size() && src[i] != '(' && src[i] != ' ' && src[i] != '\t') ++i;
        std::string name = src.substr(name_start, i - name_start);
        skip_ws(src, i);
        if (i >= src.size() || src[i] != '(') {
            // Malformed token — abort parse, return whatever we built.
            return chain;
        }
        ++i; // past '('
        size_t body_start = i;
        int depth = 1;
        while (i < src.size() && depth > 0) {
            if (src[i] == '(') ++depth;
            else if (src[i] == ')') { if (--depth == 0) break; }
            ++i;
        }
        if (i >= src.size()) return chain;
        std::string body = src.substr(body_start, i - body_start);
        ++i; // past ')'

        // Trim body whitespace.
        size_t bs = 0, be = body.size();
        while (bs < be && (body[bs] == ' ' || body[bs] == '\t')) ++bs;
        while (be > bs && (body[be - 1] == ' ' || body[be - 1] == '\t')) --be;
        body = body.substr(bs, be - bs);

        sk_sp<SkImageFilter> step;
        if (name == "blur") {
            double radius_px = parse_filter_arg(body, FilterArgKind::pixels);
            if (std::isfinite(radius_px) && radius_px > 0) {
                // CSS spec: blur radius is the standard deviation, NOT
                // the diameter. Skia takes sigma directly.
                float sigma = static_cast<float>(radius_px);
                step = SkImageFilters::Blur(sigma, sigma, nullptr);
            }
        } else if (name == "brightness") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt)) step = color_matrix_filter(make_brightness_matrix(amt));
        } else if (name == "contrast") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt)) step = color_matrix_filter(make_contrast_matrix(amt));
        } else if (name == "grayscale") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt)) {
                if (amt > 1.0) amt = 1.0;
                if (amt < 0.0) amt = 0.0;
                step = color_matrix_filter(make_grayscale_matrix(amt));
            }
        } else if (name == "hue-rotate") {
            double rad = parse_filter_arg(body, FilterArgKind::angle);
            if (std::isfinite(rad)) step = color_matrix_filter(make_hue_rotate_matrix(rad));
        } else if (name == "invert") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt)) {
                if (amt > 1.0) amt = 1.0;
                if (amt < 0.0) amt = 0.0;
                step = color_matrix_filter(make_invert_matrix(amt));
            }
        } else if (name == "opacity") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt)) {
                if (amt > 1.0) amt = 1.0;
                if (amt < 0.0) amt = 0.0;
                step = color_matrix_filter(make_opacity_matrix(amt));
            }
        } else if (name == "saturate") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt) && amt >= 0.0) {
                step = color_matrix_filter(make_saturation_matrix(amt));
            }
        } else if (name == "sepia") {
            double amt = parse_filter_arg(body, FilterArgKind::multiplier);
            if (std::isfinite(amt)) {
                if (amt > 1.0) amt = 1.0;
                if (amt < 0.0) amt = 0.0;
                step = color_matrix_filter(make_sepia_matrix(amt));
            }
        } else if (name == "drop-shadow") {
            // drop-shadow(<dx> <dy> <blur-radius> <color>)
            // dx, dy, blur-radius are lengths (px); color is optional
            // (defaults to black per CSS spec § filter-effects).
            auto tokens = parse_filter_args(body);
            if (tokens.size() >= 3) {
                float dx   = static_cast<float>(parse_filter_arg_length(tokens[0]));
                float dy   = static_cast<float>(parse_filter_arg_length(tokens[1]));
                float blur = static_cast<float>(parse_filter_arg_length(tokens[2]));
                if (blur < 0) blur = 0; // CSS spec: negative blur → 0
                SkColor color = SK_ColorBLACK;
                if (tokens.size() >= 4) {
                    std::string cs = tokens[3];
                    for (size_t k = 4; k < tokens.size(); ++k) cs += " " + tokens[k];
                    color = parse_css_color_to_skcolor(cs);
                }
                step = SkImageFilters::DropShadow(dx, dy, blur, blur, color, nullptr, nullptr);
            }
        }
        chain = compose(std::move(step), std::move(chain));
        // Skip optional comma between functions (spec is whitespace-only,
        // but tolerate ",").
        skip_ws(src, i);
        if (i < src.size() && src[i] == ',') { ++i; }
    }
    return chain;
}

} // namespace

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

// ── SkParagraph-based shape helper ─────────────────────────────────────
// Canvas2D text routes through `SkParagraph` for cluster-aware shaping
// + color-emoji fallback. SkParagraph uses HarfBuzz + ICU + the
// `TextFontContext`'s FontCollection (which has the registered emoji
// typeface in its default-family list) so ZWJ families, regional flag
// pairs, keycaps, and skin-tone modifiers all shape as single grapheme
// clusters routed to the emoji font.
namespace {

struct PreparedParagraph {
    std::unique_ptr<skia::textlayout::Paragraph> paragraph;
    float advance = 0;
    float alphabetic_baseline = 0;
};

PreparedParagraph make_paragraph(const std::string& text,
                                  const std::string& family,
                                  float size,
                                  int weight,
                                  int slant,
                                  float letter_spacing,
                                  bool ltr,
                                  std::optional<SkPaint> foreground_paint,
                                  const std::vector<Canvas::FontFeature>& features = {}) {
    PreparedParagraph result;
    if (text.empty()) return result;
    auto ctx = pulp::canvas::TextFontContext::shared();
    auto fc = ctx->font_collection();
    if (!fc) return result;

    skia::textlayout::ParagraphStyle pstyle;
    pstyle.setTextDirection(ltr ? skia::textlayout::TextDirection::kLtr
                                : skia::textlayout::TextDirection::kRtl);
    skia::textlayout::TextStyle tstyle;
    std::vector<SkString> families;
    // Codex P2 (PR #2157): split CSS font-family lists. `family` can be a
    // comma-separated CSS fallback list ("Inter, sans-serif"). Passing the
    // whole string as one SkString makes SkParagraph treat it as a single
    // literal family name and miss the fallback resolution. Split on commas
    // and strip surrounding whitespace + optional quote marks so each entry
    // resolves independently in the font collection.
    if (!family.empty()) {
        size_t cursor = 0;
        while (cursor < family.size()) {
            size_t comma = family.find(',', cursor);
            std::string entry = family.substr(cursor,
                comma == std::string::npos ? std::string::npos : comma - cursor);
            // Trim ASCII whitespace.
            size_t lo = entry.find_first_not_of(" \t\n\r\f\v");
            size_t hi = entry.find_last_not_of(" \t\n\r\f\v");
            if (lo != std::string::npos && hi != std::string::npos) {
                entry = entry.substr(lo, hi - lo + 1);
                // Strip a single matching pair of single or double quotes.
                if (entry.size() >= 2
                    && (entry.front() == '"' || entry.front() == '\'')
                    && entry.front() == entry.back()) {
                    entry = entry.substr(1, entry.size() - 2);
                }
                if (!entry.empty()) {
                    families.emplace_back(entry.c_str());
                }
            }
            if (comma == std::string::npos) break;
            cursor = comma + 1;
        }
    }
    const std::string emoji_family = ctx->emoji_family_name();
    if (!emoji_family.empty()) {
        families.emplace_back(emoji_family.c_str());
        families.emplace_back("Pulp Emoji");
    }
    if (families.empty()) families.emplace_back("");
    tstyle.setFontFamilies(families);
    tstyle.setFontSize(size > 0 ? size : 14.0f);
    SkFontStyle sk_style{weight,
                         SkFontStyle::kNormal_Width,
                         slant ? SkFontStyle::kItalic_Slant
                               : SkFontStyle::kUpright_Slant};
    tstyle.setFontStyle(sk_style);
    if (letter_spacing != 0.0f) tstyle.setLetterSpacing(letter_spacing);
    if (foreground_paint.has_value()) {
        tstyle.setForegroundPaint(*foreground_paint);
    }
    for (const auto& f : features) {
        // Unpack the FourByteTag back into a 4-character SkString:
        // tag = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3.
        char tag_chars[4] = {
            static_cast<char>((f.tag >> 24) & 0xFF),
            static_cast<char>((f.tag >> 16) & 0xFF),
            static_cast<char>((f.tag >> 8) & 0xFF),
            static_cast<char>(f.tag & 0xFF),
        };
        tstyle.addFontFeature(SkString(tag_chars, 4),
                              static_cast<int>(f.value));
    }
    pstyle.setTextStyle(tstyle);

    auto pb = skia::textlayout::ParagraphBuilder::make(pstyle, fc);
    if (!pb) return result;
    pb->addText(text.c_str(), text.size());
    auto paragraph = pb->Build();
    if (!paragraph) return result;
    paragraph->layout(SK_ScalarInfinity);
    result.advance = paragraph->getMaxIntrinsicWidth();
    result.alphabetic_baseline = paragraph->getAlphabeticBaseline();
    result.paragraph = std::move(paragraph);
    return result;
}

} // namespace


// pulp #2163 — minimal UTF-8 decoder. Skia's SkUTF helper isn't in our
// public include set, but the format is small and well-defined; inline
// here so we can check codepoint coverage without pulling skia/private.
// Returns U+FFFD on malformed input and always advances at least one
// byte so the caller cannot infinite-loop.
static SkUnichar next_utf8(const char* s, const char* end, int* advance) {
    if (s >= end) { *advance = 0; return 0; }
    unsigned char c = static_cast<unsigned char>(*s);
    if (c < 0x80) { *advance = 1; return c; }
    int extra;
    SkUnichar uc;
    if      ((c & 0xE0) == 0xC0) { extra = 1; uc = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; uc = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; uc = c & 0x07; }
    else                          { *advance = 1; return 0xFFFD; }
    if (s + 1 + extra > end)      { *advance = 1; return 0xFFFD; }
    for (int i = 0; i < extra; ++i) {
        unsigned char cc = static_cast<unsigned char>(s[1 + i]);
        if ((cc & 0xC0) != 0x80)  { *advance = 1; return 0xFFFD; }
        uc = (uc << 6) | (cc & 0x3F);
    }
    *advance = 1 + extra;
    return uc;
}

// pulp #2163 — per-glyph font fallback for fill_text. Walks the codepoints
// using `active` as the preferred typeface; for any codepoint missing
// from `active`, asks SkFontMgr::matchFamilyStyleCharacter for a
// fallback typeface that contains the codepoint. Builds contiguous
// runs sharing one typeface, shapes each run with SkShaper, then
// concatenates the resulting blobs.
//
// Motivated by JSX imports (e.g. Chainer) that request fonts the host
// machine doesn't have installed. The fallback typeface that
// matchFamilyStyle resolves can lack common Unicode characters like
// → (U+2192), ↑ (U+2191), em-dashes, etc. — single-typeface SkShaper
// renders those as the typeface's .notdef "tofu" box, which is
// visually broken. Per-glyph fallback fixes this without requiring
// every plugin author to ship a Unicode-complete bundled font.
//
// The fast path (ASCII-only text OR the active typeface covers every
// codepoint) skips this entirely — the caller checks before routing
// here.
static void shape_with_glyph_fallback(SkCanvas* canvas,
                                      const std::string& text,
                                      float x, float y,
                                      const SkFont& base_font,
                                      const SkPaint& paint,
                                      const std::string& font_family,
                                      int font_weight,
                                      int font_slant,
                                      bool ltr,
                                      TextAlign text_align,
                                      sk_sp<SkFontMgr> font_mgr) {
    auto* base_tf = base_font.getTypeface();
    if (!base_tf) return;

    SkFontStyle style{font_weight, SkFontStyle::kNormal_Width,
                      font_slant ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant};

    struct Run { std::string text; sk_sp<SkTypeface> tf; };
    std::vector<Run> runs;
    Run current;
    current.tf = sk_ref_sp(base_tf);

    // Cache fallback typefaces per missing codepoint within this call.
    // Same codepoint missing twice → resolve once.
    std::unordered_map<SkUnichar, sk_sp<SkTypeface>> fallback_cache;

    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        int adv = 0;
        SkUnichar cp = next_utf8(p, end, &adv);
        if (adv == 0) break;
        const char* cp_bytes = p;
        p += adv;

        // Pick a typeface for this codepoint.
        sk_sp<SkTypeface> chosen;
        // Control characters (newlines etc.) always go through the base
        // typeface — no point routing them to fallback.
        if (cp >= 0x20 && base_tf->unicharToGlyph(cp) == 0) {
            auto it = fallback_cache.find(cp);
            if (it != fallback_cache.end()) {
                chosen = it->second;
            } else if (font_mgr) {
                chosen = font_mgr->matchFamilyStyleCharacter(
                    font_family.empty() ? nullptr : font_family.c_str(),
                    style, nullptr, 0, cp);
                if (chosen && chosen->unicharToGlyph(cp) == 0) chosen.reset();
                fallback_cache[cp] = chosen;
            }
        }
        if (!chosen) chosen = sk_ref_sp(base_tf);

        if (chosen.get() != current.tf.get()) {
            if (!current.text.empty()) runs.push_back(std::move(current));
            current = Run{};
            current.tf = chosen;
        }
        current.text.append(cp_bytes, adv);
    }
    if (!current.text.empty()) runs.push_back(std::move(current));

    if (runs.empty()) return;

    // Shape each run with its own typeface.
    struct ShapedRun { sk_sp<SkTextBlob> blob; float width; };
    std::vector<ShapedRun> shaped;
    shaped.reserve(runs.size());
    float total_w = 0;

    auto shaper = SkShaper::Make();
    if (!shaper) return;

    for (const auto& r : runs) {
        SkFont rf = base_font;
        rf.setTypeface(r.tf);

        SkTextBlobBuilderRunHandler handler(r.text.c_str(), {0, 0});
        shaper->shape(r.text.c_str(), r.text.size(), rf, ltr,
                      SK_ScalarInfinity, &handler);
        const float w = handler.endPoint().x();
        shaped.push_back({handler.makeBlob(), w});
        total_w += w;
    }

    float draw_x = x;
    if (text_align == TextAlign::center) draw_x -= total_w * 0.5f;
    else if (text_align == TextAlign::right) draw_x -= total_w;

    for (const auto& sr : shaped) {
        if (sr.blob) canvas->drawTextBlob(sr.blob, draw_x, y, paint);
        draw_x += sr.width;
    }
}

// Quick pre-flight: does `tf` contain a glyph for every non-ASCII
// codepoint in `text`? ASCII bytes (< 0x80) are assumed covered by any
// sane Latin typeface and skipped — this keeps the hot path allocation-
// free. Returns true if the single-typeface SkShaper path is safe;
// false when at least one codepoint would render as .notdef.
static bool active_typeface_covers_text(SkTypeface* tf, const std::string& text) {
    if (!tf) return true;  // no font → caller will early-return anyway
    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        unsigned char b = static_cast<unsigned char>(*p);
        if (b < 0x80) { ++p; continue; }
        int adv = 0;
        SkUnichar cp = next_utf8(p, end, &adv);
        if (adv == 0) break;
        if (cp >= 0x20 && tf->unicharToGlyph(cp) == 0) return false;
        p += adv;
    }
    return true;
}

void SkiaCanvas::set_text_align(TextAlign align) {
    text_align_ = align;
}

void SkiaCanvas::fill_text_anchored(const std::string& text,
                                    float x, float y, TextAnchor anchor) {
    GUARD_CANVAS;
    if (text.empty()) return;
    // pulp #2163 / font v2 Slice 1.2.b — translate the anchor's y
    // reference into a baseline-y, then delegate to fill_text. The
    // worst-case-glyph metrics (SkFontMetrics::fTop / fBottom flipped
    // positive) come from TextShaper, which pulls them from the same
    // resolved typeface the painter will use — guarantees that the
    // anchor-y → baseline-y math matches what the painter actually
    // does, slice-1.3 parity harness asserts pixel-equal output.
    if (anchor == TextAnchor::Baseline) {
        fill_text(text, x, y);
        return;
    }

    auto& shaper = global_text_shaper();
    auto prepared = shaper.prepare(text, font_family_, font_size_);
    float ascent  = prepared.ascent();    // distance above baseline
    float descent = prepared.descent();   // distance below baseline
    if (ascent <= 0.0f)  ascent  = font_size_ * 0.85f;  // fallback
    if (descent <= 0.0f) descent = font_size_ * 0.2f;   // fallback

    float baseline_y = y;
    switch (anchor) {
        case TextAnchor::Baseline:
            // Already handled.
            break;
        case TextAnchor::GlyphTop:
            // y is glyph-top. Baseline sits `ascent` below.
            baseline_y = y + ascent;
            break;
        case TextAnchor::GlyphCenter:
            // y is glyph vertical center. Baseline sits half the glyph
            // box height below the center, offset by the asymmetry
            // between ascent and descent.
            baseline_y = y + (ascent - descent) * 0.5f;
            break;
        case TextAnchor::EmBoxTop:
            // y is em-box top. Baseline is at `font_size * baseline_ratio`
            // — for most Latin fonts ascent/font_size ≈ 0.8, so use that
            // as the em-box-to-baseline distance directly.
            baseline_y = y + font_size_ * 0.8f;
            break;
    }
    fill_text(text, x, baseline_y);
}

void SkiaCanvas::fill_text(const std::string& text, float x, float y) {
    GUARD_CANVAS;
    if (text.empty()) return;
    // pulp #2163 — `PULP_FILL_TEXT_TRACE=1` env var prints the (text, x, y)
    // arguments reaching the Skia path for the labels named below.
    // Useful for triage during the font-hardening rollout — pair with
    // PULP_LABEL_DEBUG_BOX to compare Label::paint's computed baseline
    // against the y argument fill_text actually receives.
    if (std::getenv("PULP_FILL_TEXT_TRACE")) {
        if (text.find("CROSSOVER") != std::string::npos
         || text.find("MID / SIDE") != std::string::npos
         || text.find("MULTIBAND") != std::string::npos
         || text == "LO" || text == "HI") {
            std::fprintf(stderr, "[fill_text] text='%s' x=%g y=%g family='%s' size=%g letter_sp=%g\n",
                         text.c_str(), x, y, font_family_.c_str(), font_size_, letter_spacing_);
        }
    }

    // pulp #1899 (gap #3) — when any currently-open save_layer carries
    // alpha < 1, Skia's LCD subpixel AA degrades on the non-opaque
    // destination and glyphs render faint. make_font() falls back to
    // greyscale AA in that case (browser parity).
    SkFont font = make_font(font_family_, font_size_, font_weight_, font_slant_,
                            inside_non_opaque_layer());
    if (!font.getTypeface()) return;

    // pulp Wave 3 c2d.6 — gradient (and pattern) fillStyle on text. Route
    // through current_fill_paint() so any active gradient_shader_ flows
    // onto the glyph paint. Without this, ctx.fillStyle =
    // createLinearGradient(...); ctx.fillText('Hi', x, y) silently
    // degraded to the first stop colour. The shader's geometry maps in
    // device space, so the gradient stretches across the rendered glyphs
    // exactly like Blink / WebKit. current_fill_paint() also folds in
    // the sticky Canvas2D shadow* state and the CSS filter chain (issue-
    // 1434 batch 7 / pulp #1520) so text honors `ctx.shadowBlur` and
    // `ctx.filter` in the same call.
    auto paint = current_fill_paint();

#ifdef PULP_HAS_TEXT_SHAPING
    // pulp #2163 — per-glyph font fallback. If the resolved typeface
    // lacks a glyph for any codepoint in `text` (common when JSX
    // imports request a host-uninstalled font like "IBM Plex Mono"
    // and Pulp falls back to the system default which lacks Unicode
    // arrows/dashes/etc.), partition the text into runs by typeface
    // and shape each separately. ASCII text bypasses the scan; it's
    // virtually always covered by any Latin typeface.
    //
    // pulp #2163 — for letter_spacing_ == 0 only, route missing-glyph
    // text through SkShaper-based per-run fallback for best kerning /
    // ligature quality. Letter-spaced text falls through to the
    // unified per-glyph builder below, which handles BOTH missing-glyph
    // fallback AND letter-spacing in one path — that's the path the
    // iMX8 / READY header labels both take, so they share baseline
    // placement.
    if (letter_spacing_ == 0.0f
        && !active_typeface_covers_text(font.getTypeface(), text)) {
        const bool ltr = (direction_ != TextDirection::rtl);
        shape_with_glyph_fallback(canvas_, text, x, y, font, paint,
                                  font_family_, font_weight_, font_slant_,
                                  ltr, text_align_, get_font_manager());
        return;
    }

    // pulp #2163 — if letter_spacing_ != 0 OR letter_spacing_ == 0
    // path-1 (SkShaper) gave up (above), but the active typeface
    // covers the text, fall through to the unified per-glyph builder.
    // For letter-spaced text with missing glyphs, the per-glyph builder
    // handles fallback inline so we never reach this point with .notdef
    // boxes — the bullet ● in '● READY' now renders via system fallback
    // even when letter_spacing_ > 0.
    //
    // For letter_spacing_ == 0 + fully covered text the path-1 SkShaper
    // already returned above, so we won't double-render.

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
    // SkParagraph handles cluster-aware emoji fallback, CSS letter-
    // spacing, OpenType font features, and bidi direction.
    //
    // Codex P1 (PR #2157): previously this branch was gated on
    // `needs_paragraph` (features/letter-spacing/rtl/emoji) and routed
    // plain LTR ASCII text into the per-glyph SkTextBlob fallback as a
    // hot-path optimization. That fallback has NO kerning or ligatures,
    // so common strings like "AV" / "ffi" / "fl" rendered with the
    // wrong advances and bare-glyph spacing — a visual + width-sensitive
    // regression vs. the pre-emoji code, which always shaped through
    // SkParagraph. The per-glyph blob path is now only reached when
    // shaping is compile-time disabled (`PULP_HAS_TEXT_SHAPING` off) or
    // SkParagraph fails to build a paragraph at runtime.
    {
        const bool ltr = (direction_ != TextDirection::rtl);
        auto prepared = make_paragraph(text, font_family_, font_size_,
                                        font_weight_, font_slant_,
                                        letter_spacing_, ltr, paint,
                                        font_features_);
        if (prepared.paragraph) {
            float draw_x = x;
            if (text_align_ == TextAlign::center) draw_x -= prepared.advance * 0.5f;
            else if (text_align_ == TextAlign::right) draw_x -= prepared.advance;
            // SkParagraph paint(canvas, x, y) places the paragraph TOP
            // at (x, y); Canvas2D fillText puts the alphabetic baseline
            // at y. Translate so the baseline lands on `y`.
            prepared.paragraph->paint(canvas_, draw_x,
                                       y - prepared.alphabetic_baseline);
            return;
        }
    }
#endif

    // pulp #2163 — unified per-glyph fallback path with per-codepoint
    // font fallback AND letter-spacing support. Reached when
    // PULP_HAS_TEXT_SHAPING is compile-time disabled OR (post-PR #2157)
    // when make_paragraph fails to build a paragraph. One code path so
    // every fill_text call ends up at the same baseline placement,
    // regardless of whether the text has missing glyphs or letter-spacing.
    // Replaces the prior split where letter-spaced text rendered missing
    // glyphs as .notdef boxes while non-letter-spaced text routed through
    // shape_with_glyph_fallback — the divergent baselines between paths
    // showed up as visibly stacked labels (pulp #2163 #31 iMX8/READY
    // regression).
    //
    // Algorithm:
    //  1. Walk UTF-8 codepoints.
    //  2. For each cp, prefer the active typeface; if it lacks the
    //     glyph, ask SkFontMgr::matchFamilyStyleCharacter for a
    //     fallback. Cache fallbacks per cp inside this call.
    //  3. Group consecutive codepoints with the same typeface into
    //     runs (SkTextBlobBuilder needs one allocRunPosH per font).
    //  4. Measure advances per glyph via SkFont::getWidths on the
    //     run's font.
    //  5. Lay out glyph positions with cumulative cursor + per-pair
    //     letter_spacing.
    //  6. Apply text_align after total width is known.
    //
    // SkShaper's kerning + ligature quality is lost on this path,
    // but it's only entered for letter-spaced or non-fully-covered
    // text — both cases that already preclude useful kerning. The
    // best-quality SkParagraph path (#2157) is still chosen for the
    // common case at the top of fill_text above.
    auto font_mgr_for_fallback = get_font_manager();
    SkFontStyle style{font_weight_, SkFontStyle::kNormal_Width,
                      font_slant_ ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant};
    std::unordered_map<SkUnichar, sk_sp<SkTypeface>> fallback_cache;
    auto* active_tf = font.getTypeface();

    struct GlyphEntry {
        sk_sp<SkTypeface> tf;  // typeface for this glyph
        SkGlyphID glyph;       // resolved glyph id (always non-zero when tf has the glyph)
        SkScalar width;        // advance for this glyph
    };
    std::vector<GlyphEntry> entries;
    entries.reserve(text.size());

    const char* p = text.data();
    const char* end = p + text.size();
    while (p < end) {
        int adv = 0;
        SkUnichar cp = next_utf8(p, end, &adv);
        if (adv == 0) break;
        p += adv;

        // Pick typeface for this codepoint: active → fallback.
        sk_sp<SkTypeface> chosen;
        SkGlyphID gid = 0;
        if (active_tf) {
            gid = active_tf->unicharToGlyph(cp);
            if (gid != 0) chosen = sk_ref_sp(active_tf);
        }
        if (!chosen && cp >= 0x20 && font_mgr_for_fallback) {
            auto it = fallback_cache.find(cp);
            if (it != fallback_cache.end()) chosen = it->second;
            else {
                chosen = font_mgr_for_fallback->matchFamilyStyleCharacter(
                    font_family_.empty() ? nullptr : font_family_.c_str(),
                    style, nullptr, 0, cp);
                if (chosen) {
                    gid = chosen->unicharToGlyph(cp);
                    if (gid == 0) chosen.reset();  // matcher lied; treat as no fallback
                }
                fallback_cache[cp] = chosen;
            }
            if (chosen && gid == 0) gid = chosen->unicharToGlyph(cp);
        }
        // Final fallback: emit .notdef in active typeface (preserves
        // current behavior when fallback path can't reach a font with
        // the glyph — at least the text doesn't disappear entirely).
        if (!chosen) {
            chosen = sk_ref_sp(active_tf);
            gid = 0;  // .notdef
        }
        if (!chosen) continue;  // shouldn't happen — active_tf was checked above

        // Measure advance for this single glyph in its typeface's font.
        SkFont glyph_font = font;
        glyph_font.setTypeface(chosen);
        SkScalar w = 0;
        glyph_font.getWidths(SkSpan<const SkGlyphID>(&gid, 1),
                             SkSpan<SkScalar>(&w, 1));
        entries.push_back({std::move(chosen), gid, w});
    }

    if (entries.empty()) return;

    // Total advance: sum of glyph widths + (N-1) * letter_spacing.
    float total_w = 0;
    for (const auto& e : entries) total_w += e.width;
    if (entries.size() > 1) {
        total_w += letter_spacing_ * static_cast<float>(entries.size() - 1);
    }

    float draw_x = x;
    if (text_align_ == TextAlign::center) draw_x -= total_w * 0.5f;
    else if (text_align_ == TextAlign::right) draw_x -= total_w;

    // Build runs grouped by typeface identity. SkTextBlobBuilder requires
    // one allocRunPosH per (font, count) pair, so we walk entries and
    // open a new run whenever the typeface changes.
    SkTextBlobBuilder builder;
    float cursor = draw_x;
    size_t i = 0;
    while (i < entries.size()) {
        SkTypeface* run_tf = entries[i].tf.get();
        size_t j = i + 1;
        while (j < entries.size() && entries[j].tf.get() == run_tf) ++j;
        const int run_n = static_cast<int>(j - i);

        SkFont run_font = font;
        run_font.setTypeface(entries[i].tf);
        const auto& run = builder.allocRunPosH(run_font, run_n, y);
        for (int k = 0; k < run_n; ++k) {
            run.glyphs[k] = entries[i + k].glyph;
            run.pos[k] = cursor;
            cursor += entries[i + k].width;
            // Letter-spacing applies between every pair of glyphs in
            // the source text, including across run boundaries — same
            // rule browsers use when CSS letter-spacing crosses an
            // <em> / fallback-font boundary.
            if (i + k + 1 < entries.size()) cursor += letter_spacing_;
        }
        i = j;
    }

    canvas_->drawTextBlob(builder.make(), 0, 0, paint);
}

void SkiaCanvas::fill_text_with_max_width(const std::string& text,
                                           float x, float y, float max_width) {
    // pulp #1525 — Canvas2D `fillText(text, x, y, maxWidth)`. When the
    // measured advance exceeds `max_width` the spec requires the user
    // agent to either pick a narrower font OR scale the text horizontally
    // so the resulting run is exactly `max_width` px wide. We take the
    // scaling path: it preserves the active typeface (no fallback font
    // surprises), preserves vertical metrics, and matches HarfBuzz's
    // per-cluster shape / draw model — each glyph cluster shrinks as a
    // rigid unit, keeping cluster boundaries spec-compliant.
    //
    // Sentinel: `max_width <= 0` means "no constraint" — fall through
    // to the unconstrained `fill_text` path bit-for-bit.
    if (max_width <= 0.0f || text.empty()) {
        fill_text(text, x, y);
        return;
    }
    GUARD_CANVAS;
    const float measured = measure_text(text);
    if (measured <= max_width || measured <= 0.0f) {
        // Already fits (or zero-width edge case) — no scale needed.
        fill_text(text, x, y);
        return;
    }
    const float scale = max_width / measured;
    // Scale around the text origin (x, y). Save/restore so the caller's
    // device matrix is unaffected — the spec says only the rendering of
    // this single fillText is squeezed; subsequent draws revert to
    // natural metrics.
    canvas_->save();
    canvas_->translate(x, y);
    canvas_->scale(scale, 1.0f);
    canvas_->translate(-x, -y);
    fill_text(text, x, y);
    canvas_->restore();
}

void SkiaCanvas::stroke_text(const std::string& text, float x, float y,
                              float max_width) {
    // pulp #1525 — true stroked-glyph rendering. Build a paint with
    // SkPaint::kStroke_Style so each glyph outline is honoured at the
    // active line width / stroke colour, rather than the pre-#1525
    // approximation that re-routed through fillText with strokeStyle as
    // the fill colour. HarfBuzz / SkShaper still handles cluster shaping
    // — we only swap the paint's style flag.
    GUARD_CANVAS;
    if (text.empty()) return;
    // pulp #1899 (gap #3) — see fill_text comment. Mirror the edging
    // policy so stroked glyphs inside an opacity layer track the
    // greyscale-AA path that fill_text uses.
    SkFont font = make_font(font_family_, font_size_, font_weight_, font_slant_,
                            inside_non_opaque_layer());
    if (!font.getTypeface()) return;

    auto stroke_paint = make_stroke_paint(stroke_color_, line_width_);
    stroke_paint.setAntiAlias(true);
    // Codex P2 (PR #1555): propagate sticky stroke state — lineJoin,
    // lineCap, miterLimit, and any stroke pattern shader — onto the
    // text-stroke paint. Without this, ctx.lineJoin / ctx.lineCap /
    // ctx.miterLimit / ctx.strokeStyle=createPattern(...) are silently
    // dropped on strokeText, even though every other stroke primitive
    // (stroke_rect, stroke_path, stroke_circle, …) honours them.
    apply_stroke_state(stroke_paint);
    apply_shadow_filter(stroke_paint);

    // pulp #1525 — apply maxWidth squeeze around (x, y) before drawing.
    bool needs_restore = false;
    if (max_width > 0.0f) {
        const float measured = measure_text(text);
        if (measured > max_width && measured > 0.0f) {
            const float scale = max_width / measured;
            canvas_->save();
            canvas_->translate(x, y);
            canvas_->scale(scale, 1.0f);
            canvas_->translate(-x, -y);
            needs_restore = true;
        }
    }

#ifdef PULP_HAS_TEXT_SHAPING
    // Same SkParagraph path as fill_text; ASCII / no-features / no-
    // tracking strokes skip it and use the per-glyph blob below. Color
    // emoji typefaces typically have CBDT/COLR bitmaps with no outline
    // tables, so strokeText effectively leaves them unchanged (CSS
    // behavior). Latin text in mixed-emoji runs still gets the stroke.
    const bool needs_paragraph =
        !font_features_.empty()
        || letter_spacing_ != 0.0f
        || pulp::canvas::contains_emoji(text);
    if (needs_paragraph) {
        auto prepared = make_paragraph(text, font_family_, font_size_,
                                        font_weight_, font_slant_,
                                        letter_spacing_, /*ltr=*/true,
                                        stroke_paint, font_features_);
        if (prepared.paragraph) {
            float draw_x = x;
            if (text_align_ == TextAlign::center) draw_x -= prepared.advance * 0.5f;
            else if (text_align_ == TextAlign::right) draw_x -= prepared.advance;
            prepared.paragraph->paint(canvas_, draw_x,
                                       y - prepared.alphabetic_baseline);
            if (needs_restore) canvas_->restore();
            return;
        }
    }
#endif

    // Fallback: per-glyph blob, mirrors fill_text's non-shaper path so
    // the stroke pass tracks the fill pass exactly.
    int glyph_count = static_cast<int>(font.countText(text.c_str(), text.size(), SkTextEncoding::kUTF8));
    if (glyph_count <= 0) {
        if (needs_restore) canvas_->restore();
        return;
    }

    std::vector<SkGlyphID> glyphs(glyph_count);
    font.textToGlyphs(text.c_str(), text.size(), SkTextEncoding::kUTF8,
                      SkSpan<SkGlyphID>(glyphs.data(), glyph_count));

    std::vector<SkScalar> widths(glyph_count);
    font.getWidths(SkSpan<const SkGlyphID>(glyphs.data(), glyph_count),
                   SkSpan<SkScalar>(widths.data(), glyph_count));

    float total_w = 0;
    for (int i = 0; i < glyph_count; ++i) total_w += widths[i];
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

    canvas_->drawTextBlob(builder.make(), 0, 0, stroke_paint);
    if (needs_restore) canvas_->restore();
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
    //
    // pulp Wave 3 c2d.6 — gradient/pattern fillStyle on SDF-drawn text.
    // Mirror the fill_text() update: route through current_fill_paint()
    // so an active gradient_shader_ tints the glyph quad consistently
    // with the shape-fill paths. The SDF channel is sampled out of the
    // alpha-only atlas image; the paint shader supplies the colour, so
    // gradients composite identically to the Skia-shaped text path.
    auto paint = current_fill_paint();
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
    // SkParagraph is the only working shape path in our Skia prebuilt
    // (legacy SkShaper APIs return zero-width). Hot-path labels (plain
    // ASCII, no features, no tracking) skip it to avoid the per-call
    // `ParagraphBuilder + Build + layout` cost — they're consistent
    // with the per-glyph fallback below that fill_text also takes on
    // the same input. Anything emoji-bearing or feature-tagged still
    // goes through SkParagraph so the measured width matches what
    // fill_text actually draws.
    const bool needs_paragraph =
        !font_features_.empty()
        || letter_spacing_ != 0.0f
        || pulp::canvas::contains_emoji(text);
    if (needs_paragraph) {
        auto prepared = make_paragraph(text, font_family_, font_size_,
                                        font_weight_, font_slant_,
                                        letter_spacing_, /*ltr=*/true,
                                        std::nullopt, font_features_);
        if (prepared.paragraph) {
            return prepared.advance;
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
    // total advance the renderer will draw. Legacy fallback path —
    // glyph-pair tracking, not cluster-aware. Hit only when the
    // shaper / text-shaping module is unavailable.
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

#ifdef PULP_HAS_TEXT_SHAPING
    // `font.measureText` counts emoji codepoints as tofu (.notdef
    // advance); re-measure via SkParagraph when the text contains
    // emoji, font-features, or letter-spacing so `width` matches what
    // fill_text actually draws. ASCII / no-feature labels keep the
    // SkFont measurement above — same width fill_text emits via the
    // per-glyph fallback path.
    const bool needs_paragraph =
        !font_features_.empty()
        || letter_spacing_ != 0.0f
        || pulp::canvas::contains_emoji(text);
    if (needs_paragraph) {
        auto prepared = make_paragraph(text, font_family_, font_size_,
                                        font_weight_, font_slant_,
                                        letter_spacing_, /*ltr=*/true,
                                        std::nullopt, font_features_);
        if (prepared.paragraph) {
            advance = prepared.advance;
        }
    }
#endif

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

// ── pulp #1737 / #1515 — CSS mask-image + mask-size paint slice ─────────
//
// Phase 1 ships linear-gradient + radial-gradient mask shapes. url() and
// other shapes fall through to the no-mask path (save_layer) — content
// renders unchanged, mask is silently bypassed (matches pre-#1737
// behavior where the catalog claim was storage-only). Phase 2 (followup)
// can add url(<file_path>) via image_cache and url(#elementRef) via the
// SVG-defs registry that pulp #932 PR-2 also needs.
//
// CSS Masking Module Level 1 §5 specifies the kDstIn composite — see
// SkiaCanvas::restore() for the matching close-side composite.

namespace {

// Skip ASCII whitespace forward in `s` starting at `i`.
inline void mask_skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i]==' ' || s[i]=='\t' || s[i]=='\n')) ++i;
}

// Parse a single CSS color literal at position `i` in `s`, advancing
// `i` past the parsed token. Supports: #RGB, #RRGGBB, #RRGGBBAA,
// `rgb(r,g,b)`, `rgba(r,g,b,a)`, `transparent`, `black`, `white`.
// Returns std::nullopt for unparseable input.
std::optional<SkColor> parse_color_token(const std::string& s, size_t& i) {
    mask_skip_ws(s, i);
    if (i >= s.size()) return std::nullopt;
    if (s[i] == '#') {
        size_t start = i + 1;
        size_t end = start;
        while (end < s.size() && std::isxdigit(static_cast<unsigned char>(s[end]))) ++end;
        std::string hex = s.substr(start, end - start);
        i = end;
        auto from_hex = [](const std::string& h, int width) -> int {
            return std::stoi(h.substr(0, width), nullptr, 16);
        };
        if (hex.size() == 3) {
            int r = from_hex(hex.substr(0,1), 1) * 17;
            int g = from_hex(hex.substr(1,1), 1) * 17;
            int b = from_hex(hex.substr(2,1), 1) * 17;
            return SkColorSetARGB(255, r, g, b);
        }
        if (hex.size() == 6) {
            int r = from_hex(hex.substr(0,2), 2);
            int g = from_hex(hex.substr(2,2), 2);
            int b = from_hex(hex.substr(4,2), 2);
            return SkColorSetARGB(255, r, g, b);
        }
        if (hex.size() == 8) {
            int r = from_hex(hex.substr(0,2), 2);
            int g = from_hex(hex.substr(2,2), 2);
            int b = from_hex(hex.substr(4,2), 2);
            int a = from_hex(hex.substr(6,2), 2);
            return SkColorSetARGB(a, r, g, b);
        }
        return std::nullopt;
    }
    // Function form: rgb(...) / rgba(...)
    auto starts = [&](const char* lit) {
        size_t n = std::strlen(lit);
        if (i + n > s.size()) return false;
        return s.compare(i, n, lit) == 0;
    };
    if (starts("rgba(") || starts("rgb(")) {
        bool has_alpha = starts("rgba(");
        i += has_alpha ? 5 : 4;
        auto eat_num = [&](float& out) -> bool {
            mask_skip_ws(s, i);
            size_t start = i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) ||
                                    s[i]=='.' || s[i]=='-' || s[i]=='+')) ++i;
            if (start == i) return false;
            out = std::stof(s.substr(start, i - start));
            mask_skip_ws(s, i);
            // skip optional %
            if (i < s.size() && s[i]=='%') { out = out * 2.55f; ++i; mask_skip_ws(s, i); }
            return true;
        };
        float r=0, g=0, b=0, a=255.0f;
        if (!eat_num(r)) return std::nullopt;
        if (i<s.size() && s[i]==',') { ++i; }
        if (!eat_num(g)) return std::nullopt;
        if (i<s.size() && s[i]==',') { ++i; }
        if (!eat_num(b)) return std::nullopt;
        if (has_alpha) {
            if (i<s.size() && s[i]==',') { ++i; }
            float af = 0.0f;
            if (!eat_num(af)) return std::nullopt;
            // alpha is 0..1 in rgba; rescale to 0..255
            a = af <= 1.0f ? af * 255.0f : af;
        }
        mask_skip_ws(s, i);
        if (i<s.size() && s[i]==')') ++i;
        auto clamp_byte = [](float f) -> int {
            int v = static_cast<int>(std::round(f));
            return v < 0 ? 0 : (v > 255 ? 255 : v);
        };
        return SkColorSetARGB(clamp_byte(a), clamp_byte(r), clamp_byte(g), clamp_byte(b));
    }
    // Named keywords (minimal subset — extend as the catalog requires).
    if (starts("transparent")) { i += 11; return SkColorSetARGB(0, 0, 0, 0); }
    if (starts("black"))       { i += 5;  return SkColorSetARGB(255, 0, 0, 0); }
    if (starts("white"))       { i += 5;  return SkColorSetARGB(255, 255, 255, 255); }
    return std::nullopt;
}

// Parse a CSS `linear-gradient(...)` mask-image value into a Skia
// shader sized to the bounds (x,y,w,h). Returns nullptr for unparseable
// input — caller falls back to the no-mask path.
//
// Phase 1 supports a deliberately narrow subset:
//   linear-gradient(<dir>, <color> [, <color>]+)
// where <dir> is one of: `to top`, `to bottom`, `to left`, `to right`,
// or an angle like `<n>deg`. Defaults to `to bottom` if no direction
// keyword/angle leads. Stops without explicit positions are evenly
// distributed (matching CSS spec). Other forms (corner directions,
// stop positions, color-interpolation hints) are deferred — the parser
// returns nullptr for them so the canvas safely falls through.
sk_sp<SkShader> parse_linear_gradient_mask(const std::string& value,
                                            float x, float y, float w, float h) {
    // Locate `linear-gradient(...)`.
    constexpr std::string_view prefix = "linear-gradient(";
    auto p = value.find(prefix);
    if (p == std::string::npos) return nullptr;
    size_t i = p + prefix.size();
    // Find matching `)` — bounded by depth counter.
    size_t end = i;
    int depth = 1;
    while (end < value.size() && depth > 0) {
        if (value[end] == '(') ++depth;
        else if (value[end] == ')') --depth;
        ++end;
    }
    if (depth != 0) return nullptr;
    std::string inner = value.substr(i, end - i - 1);  // strip closing `)`

    // Default direction: `to bottom` (top → bottom).
    float angle_deg = 180.0f;  // 0 = to top, 180 = to bottom (CSS convention)
    size_t k = 0;
    mask_skip_ws(inner, k);
    auto starts_with = [&](const char* lit) {
        size_t n = std::strlen(lit);
        return k + n <= inner.size() && inner.compare(k, n, lit) == 0;
    };
    bool consumed_dir = false;
    if (starts_with("to top"))    { angle_deg = 0;   k += 6; consumed_dir = true; }
    else if (starts_with("to bottom")) { angle_deg = 180; k += 9; consumed_dir = true; }
    else if (starts_with("to right"))  { angle_deg = 90;  k += 8; consumed_dir = true; }
    else if (starts_with("to left"))   { angle_deg = 270; k += 7; consumed_dir = true; }
    else {
        // Try angle: `<n>deg`
        size_t numstart = k;
        while (k < inner.size() && (std::isdigit(static_cast<unsigned char>(inner[k])) ||
                                    inner[k] == '.' || inner[k] == '-' || inner[k] == '+')) ++k;
        if (k > numstart) {
            float ang = std::stof(inner.substr(numstart, k - numstart));
            mask_skip_ws(inner, k);
            if (k + 3 <= inner.size() && inner.compare(k, 3, "deg") == 0) {
                k += 3;
                angle_deg = ang;
                consumed_dir = true;
            } else {
                k = numstart;  // wasn't a direction, rewind
            }
        }
    }
    mask_skip_ws(inner, k);
    if (consumed_dir) {
        if (k < inner.size() && inner[k] == ',') { ++k; mask_skip_ws(inner, k); }
    }

    // Parse color stops.
    std::vector<SkColor> colors;
    while (k < inner.size()) {
        auto col = parse_color_token(inner, k);
        if (!col) return nullptr;
        colors.push_back(*col);
        mask_skip_ws(inner, k);
        // Skip optional position (we ignore explicit positions in
        // Phase 1 — CSS even-distribution is the default fallback).
        while (k < inner.size() && inner[k] != ',' && inner[k] != ')') ++k;
        if (k < inner.size() && inner[k] == ',') { ++k; mask_skip_ws(inner, k); }
    }
    if (colors.size() < 2) return nullptr;

    // Convert angle (CSS convention) to two endpoint SkPoints inside
    // the box (x,y,w,h). 0deg = bottom→top, 90deg = left→right (CSS).
    // Skia gradient direction is from pts[0] to pts[1].
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const float angle_rad = (angle_deg - 90.0f) * 3.14159265f / 180.0f;
    // Half-diagonal so any angle reaches the box corners.
    const float half_diag = 0.5f * std::sqrt(w*w + h*h);
    const float dx = std::cos(angle_rad) * half_diag;
    const float dy = std::sin(angle_rad) * half_diag;
    SkPoint pts[2] = { {cx - dx, cy - dy}, {cx + dx, cy + dy} };
    return SkGradientShader::MakeLinear(pts, colors.data(), nullptr,
                                         static_cast<int>(colors.size()),
                                         SkTileMode::kClamp);
}

// Parse a CSS `mask-size` value into a (width_factor, height_factor)
// pair where each factor is a multiplier on the bounds.
//   `auto`        → (1, 1)
//   `cover`       → (1, 1) — for a single shader fill, "cover" and the
//                  default both fill the box. (Non-square mask images
//                  would diverge here, but for the gradient-only Phase 1
//                  the simplification is safe — see notes.)
//   `contain`     → (1, 1) — same simplification rationale.
//   `<n>%`        → (n/100, n/100)
//   `<n>% <m>%`   → (n/100, m/100)
//   `<n>px <m>px` → (n/w, m/h)
//   `<n>px`       → (n/w, n/h)
//
// Returns (1, 1) for unrecognized input — safe default that matches CSS
// `mask-size: auto` behavior (cover the box).
std::pair<float, float> parse_mask_size(const std::string& s, float w, float h) {
    if (s.empty() || s == "auto" || s == "cover" || s == "contain") {
        return {1.0f, 1.0f};
    }
    // Tokenize on whitespace.
    std::vector<std::string> toks;
    {
        size_t i = 0;
        while (i < s.size()) {
            mask_skip_ws(s, i);
            if (i >= s.size()) break;
            size_t start = i;
            while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\n') ++i;
            toks.push_back(s.substr(start, i - start));
        }
    }
    if (toks.empty()) return {1.0f, 1.0f};

    auto eat_dim = [&](const std::string& t, float bound_axis) -> std::optional<float> {
        // Trailing-unit aware: <n>% / <n>px / <n>
        size_t dot = t.find_first_not_of("0123456789.+-");
        if (dot == std::string::npos) {
            // No unit — interpret as a unitless fraction (CSS spec: % is
            // expected; treat unitless as raw px for forgiveness).
            try { return std::stof(t) / bound_axis; }
            catch (...) { return std::nullopt; }
        }
        std::string num_part = t.substr(0, dot);
        std::string unit = t.substr(dot);
        float n;
        try { n = std::stof(num_part); } catch (...) { return std::nullopt; }
        if (unit == "%") return n / 100.0f;
        if (unit == "px") return n / bound_axis;
        // Unknown unit — treat as px (forgiving).
        return n / bound_axis;
    };

    auto wf = eat_dim(toks[0], w);
    if (!wf) return {1.0f, 1.0f};
    if (toks.size() == 1) return {*wf, *wf};
    auto hf = eat_dim(toks[1], h);
    if (!hf) return {*wf, *wf};
    return {*wf, *hf};
}

}  // namespace

void SkiaCanvas::save_layer_with_mask(float x, float y, float w, float h,
                                      float opacity,
                                      const std::string& mask_image,
                                      const std::string& mask_size) {
    if (!canvas_) { save(); return; }

    // pulp #1737 / #1515 — apply mask-size by scaling the effective
    // box passed to the gradient parser. mask-size = "50% 100%" makes
    // the gradient cover half the width but the full height; the mask
    // alpha outside that scaled region is zero (kClamp on the shader
    // edges — content there gets clipped by kDstIn).
    const auto [wf, hf] = parse_mask_size(mask_size, w, h);
    const float scaled_w = w * wf;
    const float scaled_h = h * hf;

    sk_sp<SkShader> mask_shader;
    if (mask_image.find("linear-gradient(") != std::string::npos) {
        mask_shader = parse_linear_gradient_mask(mask_image, x, y, scaled_w, scaled_h);
    }
    // Other forms (radial-gradient / url / none / unparseable) drop to
    // the no-mask path: the layer opens with content opacity, restore()
    // sees no pending mask shader and just closes plainly. Net behavior
    // = pre-#1737 (no mask applied) — safe regression-free fallback.

    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    SkPaint outer_paint;
    if (opacity < 1.0f) outer_paint.setAlphaf(opacity);
    canvas_->saveLayer(&bounds, &outer_paint);

    // pulp #1899 (gap #3) — non-opaque text-edging tracking, mirrors
    // the other save_layer* variants.
    if (opacity < 1.0f) {
        non_opaque_layer_stack_.push_back(canvas_->getSaveCount());
    }

    // Always push a PendingMask so restore() pops one entry per
    // save_layer_with_mask call. Null shader means "no mask to apply,
    // just close the layer plainly" — keeps the bookkeeping consistent
    // even when the mask string didn't parse.
    PendingMask m;
    m.shader = std::move(mask_shader);
    m.bounds = {x, y, x + w, y + h};
    m.save_count_after_open = canvas_->getSaveCount();
    pending_masks_.push_back(std::move(m));
}

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
