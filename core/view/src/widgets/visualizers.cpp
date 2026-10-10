// Visualizer widget implementations. Public declarations stay in
// core/view/include/pulp/view/widgets.hpp.

#include <pulp/view/widgets.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/image_cache.hpp>
#include <pulp/view/text_overflow.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/canvas/text_shaper.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/capability_fallback.hpp>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace pulp::view {

void ImageView::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Value-driven silhouette fill: overlay fill_color_ from the bottom up to
    // fill_value_ of the height, masked to this image's own alpha via the canvas
    // url() mask, so the illustration "fills" as its bound value rises. Call
    // after a successful image draw so the fill sits over the art.
    auto emit_silhouette_fill = [&](const std::string& fs) {
        if (fill_value_ < 0.0f || fs.empty()) return;
        const float frac = std::clamp(fill_value_, 0.0f, 1.0f);
        const float fh = b.height * frac;
        if (fh <= 0.0f) return;
        canvas.save_layer_with_mask(0, 0, b.width, b.height, 1.0f,
                                    "url(" + fs + ")", "100% 100%");
        const float fill_top = b.height - fh;
        if (has_fill_gradient()) {
            // Per-shape gradient: stop[0] at the shape bottom → stop[last] at
            // the top. Draw row-by-row across the FULL shape height (so the
            // colors land where the art has them) but only paint the revealed
            // [fill_top, height) band — the same reveal model as the Meter's
            // skinned gradient. The mask clips each row to the silhouette.
            const int y0 = static_cast<int>(std::floor(fill_top));
            const int y1 = static_cast<int>(std::ceil(b.height));
            for (int y = y0; y < y1; ++y) {
                // position up the shape: 0 at the bottom, 1 at the top.
                const float t = b.height > 1.0f
                    ? 1.0f - (static_cast<float>(y) + 0.5f) / b.height
                    : 0.0f;
                canvas.set_fill_color(fill_gradient_color_at(t));
                canvas.fill_rect(0, static_cast<float>(y), b.width, 1.0f);
            }
        } else {
            canvas.set_fill_color(fill_color_);
            canvas.fill_rect(0, fill_top, b.width, fh);
        }
        canvas.restore();
    };
    auto strip_scheme = [](std::string p) {
        constexpr std::string_view kFile = "file://";
        if (p.size() >= kFile.size() && p.compare(0, kFile.size(), kFile) == 0)
            p = p.substr(kFile.size());
        return p;
    };

    if (path_.empty()) {
        // Placeholder: gray rect with "IMG" text
        canvas.set_fill_color(resolve_color("bg.surface", canvas::Color::rgba8(50, 50, 60)));
        canvas.fill_rounded_rect(0, 0, b.width, b.height, 4);
        canvas.set_fill_color(resolve_color("text.secondary", canvas::Color::rgba8(120, 120, 140)));
        canvas.set_font("Inter", 10);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text_anchored("IMG", b.width * 0.5f, b.height * 0.5f, canvas::Canvas::TextAnchor::GlyphCenter);
        return;
    }

    // When an ImageCache is attached, consult it. The cache owns decode +
    // eviction; the view just renders whatever the cache hands back. A null
    // DecodedImage means decode-in-progress or permanent failure, in which case
    // we fall through to the path-as-text placeholder so the UI never shows a
    // blank rect.
    if (cache_) {
        const auto* decoded = cache_->get(path_);
        if (decoded && decoded->native_handle) {
            // Hand the opaque native handle to the canvas. Backends that
            // know how to interpret it (CoreGraphics → CGImageRef,
            // Skia → SkImage*) blit it; others no-op and we fall through
            // to the filename placeholder.
            canvas.draw_image(decoded->native_handle, 0, 0, b.width, b.height);
            emit_silhouette_fill(strip_scheme(path_));
            return;
        }
    }

    // Decode on paint via the canvas's draw_image_from_file primitive.
    // Skia's implementation uses SkData::MakeFromFileName +
    // SkImages::DeferredFromEncodedData (deferred decode through SkCodec).
    // Backends that don't implement this primitive (some headless tests)
    // return false and we fall through to the filename-as-text placeholder
    // so authors can still see what URL is set.
    //
    // Strip a `file://` prefix if present (set_image_path normalizes file
    // paths to that form so the cache layer above can keep URI-keyed
    // entries; the canvas primitive expects a bare filesystem path).
    auto fs_path = path_;
    static const std::string kFileScheme = "file://";
    if (fs_path.compare(0, kFileScheme.size(), kFileScheme) == 0) {
        fs_path = fs_path.substr(kFileScheme.size());
    }

    // Honor CSS `object-fit` + `object-position`. Probe intrinsic image
    // dimensions; if the backend can't measure (no decode primitive on this
    // platform) fall back to the stretch-to-bounds path (= object-fit: fill).
    float img_w = 0.0f, img_h = 0.0f;
    const bool is_inline_image = path_.starts_with("data:image/");
    if (is_inline_image && cached_data_.empty()) {
        auto decoded = AssetManager::instance().load_image_from_data_uri(path_);
        if (decoded.valid()) cached_data_ = std::move(decoded.pixels);
    }
    const bool valid_inline_png = [&] {
        static constexpr uint8_t kPngSignature[] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
        if (cached_data_.size() < 24 ||
            !std::equal(std::begin(kPngSignature), std::end(kPngSignature),
                        cached_data_.begin()))
            return false;
        const auto read_be32 = [&](std::size_t offset) {
            return (static_cast<uint32_t>(cached_data_[offset]) << 24) |
                   (static_cast<uint32_t>(cached_data_[offset + 1]) << 16) |
                   (static_cast<uint32_t>(cached_data_[offset + 2]) << 8) |
                   static_cast<uint32_t>(cached_data_[offset + 3]);
        };
        img_w = static_cast<float>(read_be32(16));
        img_h = static_cast<float>(read_be32(20));
        return img_w > 0.0f && img_h > 0.0f;
    }();
    const bool has_intrinsic =
        (is_inline_image && valid_inline_png) ||
        (!is_inline_image && !fs_path.empty() &&
         canvas.measure_image_from_file(fs_path, img_w, img_h) &&
         img_w > 0.0f && img_h > 0.0f);

    struct FitRect { float x, y, width, height; };
    auto compute_fit = [&]() -> std::pair<FitRect, FitRect> {
        // dst rect (relative to view origin), src rect (in image coords).
        FitRect dst{0.0f, 0.0f, b.width, b.height};
        FitRect src{0.0f, 0.0f, img_w, img_h};
        const std::string& fit = object_fit();

        // Default `fill` (CSS spec): stretch to the box, full src.
        // Same applies when `object-fit` is unset or unknown.
        if (fit.empty() || fit == "fill") {
            return {dst, src};
        }

        // `none` — natural size, no scaling. Crop or letterbox both axes.
        // Center by default; object-position refines letterboxed placement
        // below while the crop window stays centered.
        if (fit == "none") {
            float dw = std::min(b.width,  img_w);
            float dh = std::min(b.height, img_h);
            dst = {(b.width - dw) * 0.5f, (b.height - dh) * 0.5f, dw, dh};
            float sx = (img_w - dw) * 0.5f;
            float sy = (img_h - dh) * 0.5f;
            src = {std::max(sx, 0.0f), std::max(sy, 0.0f), dw, dh};
            return {dst, src};
        }

        // `contain` (and `scale-down` when img > box) — letterbox,
        // preserve aspect ratio, scale so the larger axis hits the box.
        // `cover` — crop, preserve aspect ratio, scale so the smaller
        // axis hits the box (the other axis overflows + gets cropped).
        // `scale-down` chooses between `none` and `contain` based on
        // which produces the smaller painted region.
        bool is_contain = (fit == "contain");
        bool is_cover   = (fit == "cover");
        bool is_scale_down = (fit == "scale-down");

        if (is_scale_down) {
            // Per spec: scale-down behaves as `none` if it would shrink
            // (i.e. the image is smaller than the box on both axes);
            // otherwise behaves as `contain`.
            if (img_w <= b.width && img_h <= b.height) {
                // Recurse via the `none` path semantics.
                float dw = img_w, dh = img_h;
                dst = {(b.width - dw) * 0.5f, (b.height - dh) * 0.5f, dw, dh};
                src = {0, 0, img_w, img_h};
                return {dst, src};
            }
            is_contain = true;
        }

        if (is_contain || is_cover) {
            float scale_x = b.width  / img_w;
            float scale_y = b.height / img_h;
            float scale = is_cover ? std::max(scale_x, scale_y)
                                   : std::min(scale_x, scale_y);
            float dw = img_w * scale;
            float dh = img_h * scale;
            dst = {(b.width - dw) * 0.5f, (b.height - dh) * 0.5f, dw, dh};
            src = {0, 0, img_w, img_h};
            return {dst, src};
        }

        // Unknown fit keyword — degrade to fill.
        return {dst, src};
    };

    if (has_intrinsic) {
        auto [dst, src] = compute_fit();

        // Apply `object-position` as a percentage offset. CSS spec lets the
        // value be lengths or percentages; the JS
        // shim normalizes to a `<x>% <y>%` two-token string before
        // routing through setObjectPosition. Anything we can't parse
        // collapses to "50% 50%" (the spec default), which keeps the
        // centerd-by-default `compute_fit` result.
        auto parse_object_position = [&](float& px, float& py) {
            px = 50.0f; py = 50.0f;
            const std::string& s = object_position();
            if (s.empty()) return;
            // Tokenise on whitespace; accept `Npx`, `N%`, or bare numbers.
            std::vector<std::string> toks;
            std::string cur;
            for (char c : s) {
                if (std::isspace(static_cast<unsigned char>(c))) {
                    if (!cur.empty()) { toks.push_back(cur); cur.clear(); }
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) toks.push_back(cur);

            auto parse_float_strict = [](std::string_view text, float& out) {
                if (text.empty()) return false;
                std::string owned(text);
                char* end = nullptr;
                float value = std::strtof(owned.c_str(), &end);
                if (end == owned.c_str()) return false;
                while (*end != '\0') {
                    if (!std::isspace(static_cast<unsigned char>(*end))) return false;
                    ++end;
                }
                if (!std::isfinite(value)) return false;
                out = value;
                return true;
            };

            auto parse_one = [&](const std::string& tok, float box_extent,
                                  float img_extent, float& out_pct) {
                std::string_view body(tok);
                const bool is_percent = !body.empty() && body.back() == '%';
                const bool is_px = body.size() > 2 &&
                                   body.substr(body.size() - 2) == "px";
                if (is_percent) {
                    body.remove_suffix(1);
                } else if (is_px) {
                    body.remove_suffix(2);
                } else if (!body.empty() &&
                           std::isalpha(static_cast<unsigned char>(body.back()))) {
                    return;
                }

                float n = 0.0f;
                if (!parse_float_strict(body, n)) return;

                // Percentage: linear; CSS object-position percentage
                // means "<P% of the SLACK> measured from the start".
                if (is_percent) {
                    out_pct = n;
                    return;
                }
                // Length (px): convert to a percentage of the slack.
                float slack = box_extent - img_extent;
                if (std::abs(slack) < 0.001f) { out_pct = 50.0f; return; }
                out_pct = (n / slack) * 100.0f;
            };

            if (toks.size() >= 1) parse_one(toks[0], b.width,  dst.width,  px);
            if (toks.size() >= 2) parse_one(toks[1], b.height, dst.height, py);
        };

        if (!object_position().empty()) {
            float px = 50.0f, py = 50.0f;
            parse_object_position(px, py);
            float slack_x = b.width  - dst.width;
            float slack_y = b.height - dst.height;
            dst.x = slack_x * (px / 100.0f);
            dst.y = slack_y * (py / 100.0f);
        }

        // Route via the source-rect overload only when `none` cropped the image
        // (non-identity src). Full-source modes such as `fill`, `contain`,
        // `cover`, and `scale-down` use the dst-only path.
        bool drawn = false;
        if (src.x != 0.0f || src.y != 0.0f ||
            std::abs(src.width  - img_w) > 0.001f ||
            std::abs(src.height - img_h) > 0.001f) {
            if (is_inline_image) {
                drawn = canvas.draw_image_from_data_rect(
                    cached_data_.data(), cached_data_.size(),
                    src.x, src.y, src.width, src.height,
                    dst.x, dst.y, dst.width, dst.height);
            } else {
                drawn = canvas.draw_image_from_file_rect(
                    fs_path,
                    src.x, src.y, src.width, src.height,
                    dst.x, dst.y, dst.width, dst.height);
            }
        } else {
            if (is_inline_image) {
                drawn = canvas.draw_image_from_data(
                    cached_data_.data(), cached_data_.size(),
                    dst.x, dst.y, dst.width, dst.height);
            } else {
                drawn = canvas.draw_image_from_file(
                    fs_path, dst.x, dst.y, dst.width, dst.height);
            }
        }
        if (drawn) {
            loaded_ = true;
            emit_silhouette_fill(fs_path);
            return;
        }
    } else if (!fs_path.empty() &&
               canvas.draw_image_from_file(fs_path, 0, 0, b.width, b.height)) {
        loaded_ = true;
        emit_silhouette_fill(fs_path);
        return;
    }

    // Decode failed (file missing, unsupported codec, or backend doesn't
    // implement draw_image_from_file). Fall through to the filename
    // placeholder so authors can debug src wiring without a blank rect.
    // Distinguish the two causes for the author: when the backend has no image
    // decoder at all, the placeholder is inevitable regardless of the src, so
    // warn once (through the shared warn-once vocabulary that paint_all's
    // capability fallbacks use) instead of leaving them guessing at a bad path.
    if (!canvas.supports(canvas::CanvasCapability::images)) {
        warn_capability_fallback_once(
            canvas::CanvasCapability::images,
            "canvas backend cannot draw images; ImageView renders the "
            "filename as placeholder text");
    }
    canvas.set_fill_color(resolve_color("bg.surface", canvas::Color::rgba8(50, 50, 60)));
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 4);
    canvas.set_fill_color(resolve_color("text.secondary", canvas::Color::rgba8(120, 120, 140)));
    canvas.set_font("Inter", 9);
    canvas.set_text_align(canvas::TextAlign::center);

    // Show filename
    auto name = path_;
    auto slash = name.rfind('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    canvas.fill_text_anchored(name, b.width * 0.5f, b.height * 0.5f, canvas::Canvas::TextAnchor::GlyphCenter);
}

// ── Meter ────────────────────────────────────────────────────────────────────

// The peak / held-peak indicator lines are stroked (line width up to 2px)
// CENTERED on the meter's extreme edge coordinate (y=0 or y=height at full
// scale), so they bleed ~1px past local_bounds(); paint is unclipped
// (overflow: visible). Bounded invalidation must therefore cover a slightly
// padded rect, or that fringe is never re-cleared when the peak decays and a
// stale line persists on the static chrome. See the request_repaint(Rect)
// caller contract (out-of-bounds decoration → pass the padded rect).

canvas::Color ImageView::fill_gradient_color_at(float t) const {
    if (fill_gradient_.empty()) return fill_color_;
    if (fill_gradient_.size() == 1) return fill_gradient_.front();
    t = std::clamp(t, 0.0f, 1.0f);
    const float scaled = t * static_cast<float>(fill_gradient_.size() - 1);
    int i = static_cast<int>(scaled);
    if (i >= static_cast<int>(fill_gradient_.size()) - 1)
        return fill_gradient_.back();
    const float frac = scaled - static_cast<float>(i);
    return fill_gradient_[i].interpolate(fill_gradient_[i + 1], frac);
}


void XYPad::update_from_pos(Point pos) {
    auto b = local_bounds();
    if (b.width <= 0 || b.height <= 0) return;
    float new_x = std::clamp(pos.x / b.width, 0.0f, 1.0f);
    float new_y = std::clamp(1.0f - pos.y / b.height, 0.0f, 1.0f);
    if (new_x != x_ || new_y != y_) {
        x_ = new_x;
        y_ = new_y;
        if (on_change) on_change(x_, y_);
    }
}

void XYPad::on_mouse_down(Point pos) {
    if (!dragging_ && on_gesture_begin) on_gesture_begin();
    dragging_ = true;
    update_from_pos(pos);
}

void XYPad::on_mouse_drag(Point pos) {
    if (!dragging_) return;
    update_from_pos(pos);
}

void XYPad::on_mouse_up(Point) {
    if (!dragging_) return;
    dragging_ = false;
    if (on_gesture_end) on_gesture_end();
}

void XYPad::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Background
    auto bg = resolve_color("bg.surface", canvas::Color::rgba8(40, 40, 55));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 4.0f);

    // Grid lines — a 4×4 grid (matches the Figma XY pad), not just a center cross.
    auto grid = resolve_color("waveform.grid", canvas::Color::rgba8(60, 60, 75));
    canvas.set_stroke_color(grid);
    canvas.set_line_width(0.5f);
    for (int i = 1; i < 4; ++i) {
        float gx = b.width * (static_cast<float>(i) / 4.0f);
        float gy = b.height * (static_cast<float>(i) / 4.0f);
        canvas.stroke_line(gx, 0, gx, b.height);
        canvas.stroke_line(0, gy, b.width, gy);
    }

    // Crosshair position — inset by dot radius so it doesn't clip at edges.
    // One puck: at the position playing while a modulator moves either axis.
    float dot_r = 4.0f;
    const auto at_x = [&](float v) { return dot_r + v * (b.width - 2.0f * dot_r); };
    const auto at_y = [&](float v) { return dot_r + (1.0f - v) * (b.height - 2.0f * dot_r); };
    float cx = at_x(modulated_display_x());
    float cy = at_y(modulated_display_y());

    // Crosshair lines (accent; the modulation colour while modulated)
    auto hair_color = has_modulated_value()
                          ? resolve_color("knob.modulation", canvas::Color::rgba8(190, 150, 255))
                          : resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
    canvas.set_stroke_color(hair_color);
    canvas.set_line_width(1.0f);
    canvas.stroke_line(cx, 0, cx, b.height);
    canvas.stroke_line(0, cy, b.width, cy);

    // Thumb dot — teal/accent, matching the Figma XY pad (was a white dot).
    canvas.set_fill_color(hair_color);
    canvas.fill_circle(cx, cy, dot_r);

    // The base a modulator moves the puck around: a small quiet ring.
    if (has_modulated_value()) {
        canvas.set_stroke_color(
            resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220)));
        canvas.set_line_width(1.0f);
        canvas.stroke_circle(at_x(x_), at_y(y_), dot_r - 1.0f);
    }

    // Labels
    auto text_color = resolve_color("text.secondary", canvas::Color::rgba8(150, 150, 150));
    canvas.set_fill_color(text_color);
    canvas.set_font("Inter", 9.0f);

    if (!x_label_.empty()) {
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text_anchored(x_label_, b.width * 0.5f, b.height - 6, canvas::Canvas::TextAnchor::Baseline);
    }
    if (!y_label_.empty()) {
        canvas.set_text_align(canvas::TextAlign::left);
        canvas.fill_text(y_label_, 2, 10);
    }
}

// ── WaveformView ─────────────────────────────────────────────────────────────


void Panel::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    auto bg = resolve_color(bg_token_, canvas::Color::rgba8(45, 45, 60));
    canvas.set_fill_color(bg);
    // Resolve the effective uniform radius (px or %).
    const float eff_radius = effective_corner_radius(b.width, b.height);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, eff_radius);

    if (border_width_ > 0) {
        auto border = resolve_color(border_token_, canvas::Color::rgba8(80, 80, 100));
        canvas.set_stroke_color(border);
        canvas.set_line_width(border_width_);
        // Inset stroke by half its width so the center lands on pixel boundaries
        // (Visage-style half-pixel alignment for crisper edges)
        float inset = border_width_ * 0.5f;
        canvas.stroke_rounded_rect(inset, inset,
                                    b.width - border_width_, b.height - border_width_,
                                    std::max(0.0f, eff_radius - inset));
    }
}

// ── SpectrogramView ──────────────────────────────────────────────────────────


} // namespace pulp::view
