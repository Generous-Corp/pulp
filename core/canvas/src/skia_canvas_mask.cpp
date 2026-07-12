// skia_canvas_mask.cpp — CSS mask-image / mask-size painting.
//
// Owns Skia-backed CSS mask-image / mask-size painting so mask-specific
// parsing and composition stay isolated from the rest of the Canvas2D
// backend.
//
// Supported mask inputs are parsed into a shader and applied on restore()
// with the CSS kDstIn composite. Unsupported or unparseable forms fall
// through to the no-mask path: content still renders, but no mask is applied.
//
// CSS Masking Module Level 1 §5 specifies the kDstIn composite — see
// SkiaCanvas::restore() in skia_canvas.cpp for the matching close-side
// composite.

// Skia headers MUST be included BEFORE pulp/canvas/skia_canvas.hpp. See
// skia_canvas.cpp's head-of-file comment for the C++ name-lookup rule that
// forces this ordering.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

#ifdef PULP_HAS_SKIA

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkTileMode.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkShader.h"
#include "include/core/SkSurface.h"
#include "include/core/SkBlendMode.h"
#include "skia_gradient_compat.hpp"
#include "include/effects/SkImageFilters.h"
#include "include/effects/SkRuntimeEffect.h"

#include <functional>

#endif  // PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>

#ifdef PULP_HAS_SKIA

namespace pulp::canvas {

// ── CSS mask-image + mask-size painting ─────────────────────────────────
//
// Supported mask inputs are parsed into a shader and applied on restore()
// with the CSS kDstIn composite. Unsupported or unparseable forms fall
// through to the no-mask path: content still renders, but no mask is applied.
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
// Supports a deliberately narrow subset:
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

    // Parse color stops. The Skia compatibility helper consumes SkColor4f
    // stops in an explicit sRGB SkColorSpace, so byte-sRGB CSS values take
    // exactly one byte-to-float conversion here.
    std::vector<SkColor4f> colors;
    while (k < inner.size()) {
        auto col = parse_color_token(inner, k);
        if (!col) return nullptr;
        colors.push_back(SkColor4f::FromColor(*col));
        mask_skip_ws(inner, k);
        // Skip optional position; CSS even-distribution is the default
        // fallback.
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
    // Null positions = even distribution.
    return skia_gradient::make_linear(pts, colors.data(), nullptr,
                                      static_cast<int>(colors.size()));
}

// Parse a CSS `mask-size` value into a (width_factor, height_factor)
// pair where each factor is a multiplier on the bounds.
//   `auto`        → (1, 1)
//   `cover`       → (1, 1) — for a single shader fill, "cover" and the
//                  default both fill the box. (Non-square mask images
//                  diverge here, but for shader masks that already fill
//                  the box the simplification is safe.)
//   `contain`     → (1, 1) — same simplification rationale.
//   `<n>%`        → (n/100, n/100)
//   `<n>% <m>%`   → (n/100, m/100)
//   `<n>px <m>px` → (n/w, m/h)
//   `<n>px`       → (n/w, n/w)
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

// url(<file>) mask: decode the image and build a shader scaled to the mask box
// so the kDstIn composite keeps painted content ONLY where the image's alpha is
// non-zero — an arbitrary-silhouette clip (the design-import shape-fill path
// masks a value-height gradient by an illustration PNG). Mirrors
// decode_pattern_image() in skia_canvas_gradients.cpp. kDecal tiling makes
// everything outside the image box fully transparent (= masked away).
sk_sp<SkShader> parse_url_image_mask(
    const std::string& value,
    float x, float y, float w, float h,
    const std::function<sk_sp<SkImage>(sk_sp<SkImage>)>& to_gpu) {
    const auto open = value.find("url(");
    if (open == std::string::npos) return nullptr;
    const auto close = value.find(')', open);
    if (close == std::string::npos) return nullptr;
    std::string path = value.substr(open + 4, close - (open + 4));
    auto trim = [](std::string& s) {
        while (!s.empty() &&
               (s.front() == ' ' || s.front() == '"' || s.front() == '\''))
            s.erase(s.begin());
        while (!s.empty() &&
               (s.back() == ' ' || s.back() == '"' || s.back() == '\''))
            s.pop_back();
    };
    trim(path);
    constexpr std::string_view kFile = "file://";
    if (path.size() >= kFile.size() && path.compare(0, kFile.size(), kFile) == 0)
        path = path.substr(kFile.size());
    if (path.empty()) return nullptr;
    auto data = SkData::MakeFromFileName(path.c_str());
    if (!data) return nullptr;
    auto image = SkImages::DeferredFromEncodedData(data);
    if (!image || image->width() <= 0 || image->height() <= 0) return nullptr;
    // A GPU backend cannot draw a raster-backed image as a shader — Graphite
    // logs "Couldn't convert SkImage to a Graphite-backed representation" and
    // silently drops the masked geometry every frame (the design-import shape
    // value-fill overlay); Ganesh drops it too. `to_gpu` is the canvas's
    // ensure_gpu_image, which uploads when a GPU backend is attached and hands
    // the deferred image straight back on raster/CPU canvases (headless
    // screenshots), where makeShader composites fine.
    image = to_gpu(std::move(image));
    if (!image) return nullptr;
    SkMatrix m = SkMatrix::Translate(x, y);
    m.preScale(w / static_cast<float>(image->width()),
               h / static_cast<float>(image->height()));
    return image->makeShader(SkTileMode::kDecal, SkTileMode::kDecal,
                             SkSamplingOptions(SkFilterMode::kLinear), &m);
}

}  // namespace

void SkiaCanvas::save_layer_with_mask(float x, float y, float w, float h,
                                      float opacity,
                                      const std::string& mask_image,
                                      const std::string& mask_size) {
    if (!canvas_) { save(); return; }

    // Apply mask-size by scaling the effective box passed to the mask shader
    // parser. mask-size = "50% 100%" makes the mask cover half the width but
    // the full height; alpha outside that scaled region is zero, so content
    // there gets clipped by kDstIn.
    const auto [wf, hf] = parse_mask_size(mask_size, w, h);
    const float scaled_w = w * wf;
    const float scaled_h = h * hf;

    sk_sp<SkShader> mask_shader;
    if (mask_image.find("linear-gradient(") != std::string::npos) {
        mask_shader = parse_linear_gradient_mask(mask_image, x, y, scaled_w, scaled_h);
    } else if (mask_image.find("url(") != std::string::npos) {
        mask_shader = parse_url_image_mask(
            mask_image, x, y, scaled_w, scaled_h,
            [this](sk_sp<SkImage> img) {
                return ensure_gpu_image(std::move(img));
            });
    }
    // Other forms (radial-gradient / none / unparseable) drop to the
    // no-mask path: the layer opens with content opacity, restore() sees
    // no pending mask shader and just closes plainly. Net behavior is the
    // no-mask fallback.

    SkRect bounds = SkRect::MakeXYWH(x, y, w, h);
    SkPaint outer_paint;
    if (opacity < 1.0f) outer_paint.setAlphaf(opacity);
    canvas_->saveLayer(&bounds, &outer_paint);

    // Non-opaque text-edging tracking mirrors the other save_layer* variants.
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

#endif  // PULP_HAS_SKIA
