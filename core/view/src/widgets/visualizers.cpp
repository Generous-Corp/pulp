// Visualizer widget implementations. Public declarations stay in
// core/view/include/pulp/view/widgets.hpp.

#include <pulp/view/widgets.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/image_cache.hpp>
#include <pulp/view/text_overflow.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/canvas/text_shaper.hpp>
#include <pulp/audio/audio_thumbnail.hpp>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace pulp::view {

// ── ImageView ────────────────────────────────────────────────────────────────

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
    // Strip a `file://` prefix if present (set_image_path normalises file
    // paths to that form so the cache layer above can keep URI-keyed
    // entries; the canvas primitive expects a bare filesystem path).
    auto fs_path = path_;
    static const std::string kFileScheme = "file://";
    if (fs_path.compare(0, kFileScheme.size(), kFileScheme) == 0) {
        fs_path = fs_path.substr(kFileScheme.size());
    }

    // Honour CSS `object-fit` + `object-position`. Probe intrinsic image
    // dimensions; if the backend can't measure (no decode primitive on this
    // platform) fall back to the stretch-to-bounds path (= object-fit: fill).
    float img_w = 0.0f, img_h = 0.0f;
    bool has_intrinsic =
        !fs_path.empty() &&
        canvas.measure_image_from_file(fs_path, img_w, img_h) &&
        img_w > 0.0f && img_h > 0.0f;

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
        // Centre by default; object-position refines letterboxed placement
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
        // shim normalises to a `<x>% <y>%` two-token string before
        // routing through setObjectPosition. Anything we can't parse
        // collapses to "50% 50%" (the spec default), which keeps the
        // centred-by-default `compute_fit` result.
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
            drawn = canvas.draw_image_from_file_rect(
                fs_path,
                src.x, src.y, src.width, src.height,
                dst.x, dst.y, dst.width, dst.height);
        } else {
            drawn = canvas.draw_image_from_file(
                fs_path, dst.x, dst.y, dst.width, dst.height);
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
namespace {
constexpr float kMeterPeakOverscan = 2.0f;
}

void Meter::set_level(float rms, float peak) {
    current_rms_ = std::clamp(rms, 0.0f, 1.0f);
    current_peak_ = std::clamp(peak, 0.0f, 1.0f);
    ballistics_.display_rms = current_rms_;
    ballistics_.display_peak = current_peak_;
    // A meter repaints every frame from its source; invalidate only its own
    // (peak-overscanned) rect so a plugin's static chrome does not re-composite
    // each level tick.
    const auto b = local_bounds();
    request_repaint(Rect{b.x - kMeterPeakOverscan, b.y - kMeterPeakOverscan,
                         b.width + 2 * kMeterPeakOverscan,
                         b.height + 2 * kMeterPeakOverscan});
}

void Meter::update(float raw_peak, float raw_rms, float dt) {
    ballistics_.update(raw_peak, raw_rms, dt);
    const auto b = local_bounds();
    request_repaint(Rect{b.x - kMeterPeakOverscan, b.y - kMeterPeakOverscan,
                         b.width + 2 * kMeterPeakOverscan,
                         b.height + 2 * kMeterPeakOverscan});
}

void Meter::set_source(std::shared_ptr<MeterSource> source, int channel) {
    source_ = std::move(source);
    source_channel_ = channel < 0 ? 0 : channel;
    if (source_) {
        try_source_subscription();
    } else {
        // Unbinding: drop the subscription so the meter stops holding the
        // editor's frames alive once nothing feeds it.
        stop_source_subscription();
    }
}

void Meter::try_source_subscription() {
    FrameClock* clock = frame_clock();
    if (source_sub_id_ != -1) {
        if (clock == subscribed_clock_) return;  // already on the right clock
        stop_source_subscription();              // clock changed → re-point below
    }
    if (!source_) return;               // nothing to read
    if (!clock) return;                 // no clock reachable yet (preview / pre-host)
    subscribed_clock_ = clock;
    source_sub_id_ = clock->subscribe([this](float dt) { return on_source_frame(dt); });
}

void Meter::stop_source_subscription() {
    if (source_sub_id_ != -1 && subscribed_clock_) {
        subscribed_clock_->unsubscribe(source_sub_id_);
    }
    source_sub_id_ = -1;
    subscribed_clock_ = nullptr;
}

bool Meter::on_source_frame(float dt) {
    // Self-heal teardown: if the source was unbound, or this meter was detached
    // / re-parented so the reachable clock no longer matches the one we
    // subscribed to, drop the subscription (return false auto-unsubscribes).
    // This covers detaches that never fire on_detached on every descendant.
    if (!source_ || frame_clock() != subscribed_clock_) {
        source_sub_id_ = -1;
        subscribed_clock_ = nullptr;
        return false;
    }
    const MeterFrame frame = source_->read();
    // Defensively bound the index by BOTH the frame's channel count and the
    // fixed array capacity — a misbehaving publisher can set `channels` past
    // kMaxChannels, and source_channel_ is caller-supplied. Never index OOB.
    const int usable = std::min(frame.channels, MeterFrame::kMaxChannels);
    if (source_channel_ >= 0 && source_channel_ < usable) {
        update(frame.peak[source_channel_], frame.rms[source_channel_], dt);
    }
    return true;
}

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

canvas::Color Meter::gradient_color_at(float t) const {
    if (gradient_stops_.empty()) return canvas::Color::rgba8(80, 200, 80);
    if (gradient_stops_.size() == 1) return gradient_stops_.front();
    t = std::clamp(t, 0.0f, 1.0f);
    float scaled = t * static_cast<float>(gradient_stops_.size() - 1);
    int i = static_cast<int>(scaled);
    if (i >= static_cast<int>(gradient_stops_.size()) - 1)
        return gradient_stops_.back();
    float frac = scaled - static_cast<float>(i);
    return gradient_stops_[i].interpolate(gradient_stops_[i + 1], frac);
}

void Meter::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    bool vert = orientation_ == Orientation::vertical;

    // ── Skinned gradient path (figma-plugin import) ────────────────────────
    // When the importer sampled the captured meter PNG's gradient, redraw it
    // procedurally CLIPPED to the current level so the fill animates with
    // set_level()/update() instead of freezing the captured image. The
    // gradient maps low(bottom)→high(top); the fill reveals it from the
    // bottom up to display_rms. This is value-driven, not a static image.
    if (has_skin_gradient()) {
        auto bg = has_skin_background_
            ? background_color_
            : resolve_color("control.track", canvas::Color::rgba8(15, 18, 23));
        canvas.set_fill_color(bg);
        canvas.fill_rounded_rect(0, 0, b.width, b.height, std::min(b.width, b.height) * 0.18f);

        float meter_length = vert ? b.height : b.width;
        float level = std::clamp(ballistics_.display_rms, 0.0f, 1.0f);
        float fill = std::clamp(std::round(level * meter_length), 0.0f, meter_length);

        // Draw the gradient row-by-row, but only up to the fill height. The
        // captured gradient stops span the bar's FULL colored range (low/green
        // at stop 0 → warm/red at the top stop). The captured asset draws that
        // entire ramp inside the FILLED region — the empty channel above the
        // fill is the dark housing, and the warm/red top stop sits at the TOP
        // of the fill (just under the peak line), not at the absolute meter top.
        // So sample the gradient across the FILL region (0 at the fill bottom,
        // 1 at the fill top) rather than against absolute meter height. That
        // makes the fill top read warm/red — matching the capture — while the
        // fill still reveals more of the SAME ramp as the level moves.
        //
        // Horizontal inset is derived from the captured bar-vs-housing ratio so
        // the colored bar reads as a narrower fill recessed inside the darker
        // housing slot (the capture's structure), not edge-to-edge paint.
        float h_inset = vert ? std::max(1.0f, b.width * (1.0f - bar_fill_ratio_) * 0.5f) : 1.0f;
        float v_inset = vert ? 1.0f : std::max(1.0f, b.height * (1.0f - bar_fill_ratio_) * 0.5f);
        if (fill > 0.0f) {
            if (vert) {
                int start = static_cast<int>(b.height - fill);
                float fill_top = static_cast<float>(start);
                float fill_span = std::max(1.0f, static_cast<float>(b.height) - fill_top);
                for (int y = start; y < static_cast<int>(b.height) - 1; ++y) {
                    // position within the FILL: 1.0 at the fill top, 0.0 at the
                    // fill bottom (the meter base).
                    float pos = 1.0f - (static_cast<float>(y) + 0.5f - fill_top) / fill_span;
                    canvas.set_fill_color(gradient_color_at(pos));
                    canvas.fill_rect(h_inset, static_cast<float>(y), b.width - 2 * h_inset, 1);
                }
            } else {
                float fill_span = std::max(1.0f, fill);
                for (int x = 0; x < static_cast<int>(fill); ++x) {
                    float pos = (static_cast<float>(x) + 0.5f) / fill_span;
                    canvas.set_fill_color(gradient_color_at(pos));
                    canvas.fill_rect(static_cast<float>(x), v_inset, 1, b.height - 2 * v_inset);
                }
            }
        }

        // Peak line at display_peak (white tick), matching the captured look.
        float peak_level = std::clamp(ballistics_.display_peak, 0.0f, 1.0f);
        if (peak_level > 0.01f) {
            float peak_pos = std::clamp(std::round(peak_level * meter_length), 0.0f, meter_length);
            canvas.set_stroke_color(canvas::Color::rgba8(255, 255, 255));
            canvas.set_line_width(2.0f);
            if (vert) {
                float y = b.height - peak_pos;
                canvas.stroke_line(h_inset, y, b.width - h_inset, y);
            } else {
                canvas.stroke_line(peak_pos, v_inset, peak_pos, b.height - v_inset);
            }
        }
        return;
    }

    if (render_style_ == WidgetRenderStyle::minimal) {
        // ── Minimal: gradient bar (green→red) matching design tool appearance ──
        auto green = resolve_color("accent.success", canvas::Color::rgba8(166, 227, 161));
        auto red = resolve_color("accent.error", canvas::Color::rgba8(243, 139, 168));
        // Simple two-stop vertical gradient approximation
        for (int y = 0; y < static_cast<int>(b.height); ++y) {
            float t = static_cast<float>(y) / b.height;
            canvas.set_fill_color(green.interpolate(red, t));
            canvas.fill_rect(0, static_cast<float>(y), b.width, 1);
        }
        return;
    }

    // Background
    auto bg = resolve_color("control.track", canvas::Color::rgba8(30, 30, 30));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 2.0f);

    float meter_length = vert ? b.height : b.width;
    auto level_to_pixels = [meter_length](float level) {
        return std::clamp(std::round(std::clamp(level, 0.0f, 1.0f) * meter_length),
                          0.0f,
                          meter_length);
    };

    // RMS fill (main body) — meter.* tokens carry the brand meter palette
    // (e.g. ink-signal's leaf green), distinct from the generic accent.* family.
    auto rms_color = resolve_color("meter.green", canvas::Color::rgba8(80, 200, 80));
    float rms_level = ballistics_.display_rms;

    // Color changes at different levels
    if (rms_level > 0.9f)
        rms_color = resolve_color("meter.red", canvas::Color::rgba8(240, 60, 60));
    else if (rms_level > 0.7f)
        rms_color = resolve_color("meter.yellow", canvas::Color::rgba8(240, 180, 60));

    canvas.set_fill_color(rms_color);
    float fill = level_to_pixels(rms_level);

    if (vert) {
        if (fill > 0.0f)
            canvas.fill_rect(1, b.height - fill, b.width - 2, fill);
    } else {
        if (fill > 0.0f)
            canvas.fill_rect(0, 1, fill, b.height - 2);
    }

    // Peak indicator line
    float peak_level = ballistics_.display_peak;
    if (peak_level > 0.01f) {
        float peak_pos = level_to_pixels(peak_level);
        if (peak_pos != fill) {
            auto peak_color = resolve_color("control.thumb", canvas::Color::rgba8(255, 255, 255));
            canvas.set_stroke_color(peak_color);
            canvas.set_line_width(1.0f);

            if (vert) {
                float y = b.height - peak_pos;
                canvas.stroke_line(1, y, b.width - 1, y);
            } else {
                canvas.stroke_line(peak_pos, 1, peak_pos, b.height - 1);
            }
        }
    }

    // Held peak indicator
    float held = ballistics_.held_peak;
    if (held > 0.01f) {
        auto held_color = canvas::Color::rgba8(255, 100, 100);
        if (held > 0.9f)
            held_color = resolve_color("accent.error", canvas::Color::rgba8(255, 50, 50));

        canvas.set_stroke_color(held_color);
        canvas.set_line_width(2.0f);

        float held_pos = level_to_pixels(held);
        if (vert) {
            float y = b.height - held_pos;
            canvas.stroke_line(0, y, b.width, y);
        } else {
            canvas.stroke_line(held_pos, 0, held_pos, b.height);
        }
    }
}

// ── XYPad ────────────────────────────────────────────────────────────────────

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

    // Grid lines — a 4×4 grid (matches the Figma XY pad), not just a centre cross.
    auto grid = resolve_color("waveform.grid", canvas::Color::rgba8(60, 60, 75));
    canvas.set_stroke_color(grid);
    canvas.set_line_width(0.5f);
    for (int i = 1; i < 4; ++i) {
        float gx = b.width * (static_cast<float>(i) / 4.0f);
        float gy = b.height * (static_cast<float>(i) / 4.0f);
        canvas.stroke_line(gx, 0, gx, b.height);
        canvas.stroke_line(0, gy, b.width, gy);
    }

    // Crosshair position — inset by dot radius so it doesn't clip at edges
    float dot_r = 4.0f;
    float cx = dot_r + x_ * (b.width - 2.0f * dot_r);
    float cy = dot_r + (1.0f - y_) * (b.height - 2.0f * dot_r);

    // Crosshair lines (accent)
    auto hair_color = resolve_color("accent.primary", canvas::Color::rgba8(100, 150, 255));
    canvas.set_stroke_color(hair_color);
    canvas.set_line_width(1.0f);
    canvas.stroke_line(cx, 0, cx, b.height);
    canvas.stroke_line(0, cy, b.width, cy);

    // Thumb dot — teal/accent, matching the Figma XY pad (was a white dot).
    canvas.set_fill_color(hair_color);
    canvas.fill_circle(cx, cy, dot_r);

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

size_t WaveformView::find_trigger_index(const float* samples, size_t count,
                                         TriggerMode mode) {
    if (mode == TriggerMode::free_run || count < 2) return 0;
    const bool want_rising = (mode == TriggerMode::rising_zero);
    for (size_t i = 1; i < count; ++i) {
        float prev = samples[i - 1];
        float curr = samples[i];
        if (want_rising) {
            if (prev <= 0.0f && curr > 0.0f) return i;
        } else {
            if (prev >= 0.0f && curr < 0.0f) return i;
        }
    }
    return 0;
}

void WaveformView::set_preview_shape(std::string_view shape) {
    std::string normalized(shape);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalized == "saw" || normalized == "sawtooth")
        preview_shape_ = PreviewShape::saw;
    else if (normalized == "sine" || normalized == "sin")
        preview_shape_ = PreviewShape::sine;
    else if (normalized == "square" || normalized == "squ")
        preview_shape_ = PreviewShape::square;
    else if (normalized == "tri" || normalized == "triangle")
        preview_shape_ = PreviewShape::triangle;
    else
        preview_shape_ = PreviewShape::none;
    request_repaint();
}

void WaveformView::apply_trigger() {
    if (trigger_mode_ == TriggerMode::free_run || samples_.size() < 2) return;
    size_t idx = find_trigger_index(samples_.data(), samples_.size(), trigger_mode_);
    if (idx == 0) return;  // no crossing — leave as-is
    std::rotate(samples_.begin(),
                samples_.begin() + static_cast<std::ptrdiff_t>(idx),
                samples_.end());
}

void WaveformView::set_data(const float* samples, size_t count) {
    samples_.assign(samples, samples + count);
    apply_trigger();
    thumbnail_owner_.reset();
    thumbnail_ = nullptr;  // raw samples win over any cached thumbnail
}

void WaveformView::set_data(std::vector<float> samples) {
    samples_ = std::move(samples);
    apply_trigger();
    thumbnail_owner_.reset();
    thumbnail_ = nullptr;
}

void WaveformView::set_thumbnail(
    std::shared_ptr<const pulp::audio::AudioThumbnail> thumb,
    uint32_t channel) {
    thumbnail_owner_ = std::move(thumb);
    thumbnail_ = thumbnail_owner_.get();
    thumbnail_channel_ = channel;
    // A live thumbnail shadows any prior raw sample buffer at paint time;
    // we drop the samples cache so memory doesn't double up.
    samples_.clear();
}

void WaveformView::set_thumbnail(const pulp::audio::AudioThumbnail* thumb,
                                 uint32_t channel) {
    set_thumbnail_borrowed(thumb, channel);
}

void WaveformView::set_thumbnail_borrowed(const pulp::audio::AudioThumbnail* thumb,
                                          uint32_t channel) {
    thumbnail_owner_.reset();
    thumbnail_ = thumb;
    thumbnail_channel_ = channel;
    // A live thumbnail shadows any prior raw sample buffer at paint time;
    // we drop the samples cache so memory doesn't double up.
    samples_.clear();
}

void WaveformView::clear_thumbnail() {
    thumbnail_owner_.reset();
    thumbnail_ = nullptr;
}

void WaveformView::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Background
    auto bg = resolve_color("bg.surface", canvas::Color::rgba8(30, 30, 40));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 2.0f);

    auto wave_color = resolve_color("waveform.line", canvas::Color::rgba8(100, 180, 250));
    auto fill_color = resolve_color("waveform.fill", canvas::Color::rgba(wave_color.r, wave_color.g, wave_color.b, 56.0f/255.0f));

    // Thumbnail path: render decimated min/max peaks straight from the cached
    // thumbnail without re-decoding audio.
    if (thumbnail_ != nullptr && !thumbnail_->empty()) {
        // Center line
        auto center_color = resolve_color("waveform.grid", canvas::Color::rgba8(50, 50, 60));
        canvas.set_stroke_color(center_color);
        canvas.set_line_width(0.5f);
        float cy = b.height * 0.5f;
        canvas.stroke_line(0, cy, b.width, cy);

        // Render one (min, max) pair per pixel column. Scratch storage is
        // retained on the view so repainting a stable-size editor does not
        // allocate every frame; cap at 4096 px to stay bounded.
        const std::size_t target_peaks =
            std::min<std::size_t>(static_cast<std::size_t>(std::max(1.0f, b.width)), 4096u);
        thumbnail_min_max_.assign(target_peaks * 2, 0.0f);
        const std::size_t produced = thumbnail_->render_min_max(
            thumbnail_channel_,
            static_cast<uint32_t>(target_peaks),
            thumbnail_min_max_.data());
        if (produced == 0) return;

        // Drive the GPU waveform shader with the per-column max so the
        // line/fill remain consistent with the live-sample path. We render
        // the absolute envelope (max(|min|, |max|)) — perceptually closest
        // to traditional audio-editor waveform displays.
        thumbnail_envelope_.resize(produced);
        for (std::size_t i = 0; i < produced; ++i) {
            const float lo = thumbnail_min_max_[i * 2 + 0];
            const float hi = thumbnail_min_max_[i * 2 + 1];
            const float amp = std::max(std::abs(lo), std::abs(hi));
            // Signed envelope: top half positive, bottom half negative would
            // require two passes through the shader. The shader already
            // handles symmetric ±amp, so we feed it max amplitude per column.
            thumbnail_envelope_[i] = hi >= -lo ? amp : -amp;
        }

        canvas::Canvas::WaveformStyle style;
        style.line_color = wave_color;
        style.fill_color = fill_color;
        style.line_thickness = 2.0f;
        style.show_fill = true;
        style.fill_center = 0.5f;

        canvas.draw_waveform(thumbnail_envelope_.data(), thumbnail_envelope_.size(),
                              0, 0, b.width, b.height, style);
        return;
    }

    if (samples_.empty() && preview_shape_ != PreviewShape::none) {
        auto center_color = resolve_color("waveform.grid", canvas::Color::rgba8(50, 50, 60));
        canvas.set_stroke_color(center_color);
        canvas.set_line_width(0.5f);
        const float cy = b.height * 0.5f;
        canvas.stroke_line(0, cy, b.width, cy);

        canvas.set_stroke_color(wave_color.with_alpha(wave_color.a * 0.85f));
        canvas.set_line_width(1.2f);
        canvas.begin_path();
        switch (preview_shape_) {
            case PreviewShape::saw:
                canvas.move_to(0.0f, b.height * 0.8f);
                canvas.line_to(b.width * 0.25f, b.height * 0.2f);
                canvas.line_to(b.width * 0.25f, b.height * 0.8f);
                canvas.line_to(b.width * 0.5f, b.height * 0.2f);
                canvas.line_to(b.width * 0.5f, b.height * 0.8f);
                canvas.line_to(b.width * 0.75f, b.height * 0.2f);
                canvas.line_to(b.width * 0.75f, b.height * 0.8f);
                canvas.line_to(b.width, b.height * 0.2f);
                break;
            case PreviewShape::sine:
                canvas.move_to(0.0f, b.height * 0.5f);
                canvas.quad_to(b.width * 0.12f, b.height * 0.1f, b.width * 0.25f, b.height * 0.5f);
                canvas.quad_to(b.width * 0.38f, b.height * 0.9f, b.width * 0.5f, b.height * 0.5f);
                canvas.quad_to(b.width * 0.62f, b.height * 0.1f, b.width * 0.75f, b.height * 0.5f);
                canvas.quad_to(b.width * 0.88f, b.height * 0.9f, b.width, b.height * 0.5f);
                break;
            case PreviewShape::square:
                canvas.move_to(0.0f, b.height * 0.2f);
                canvas.line_to(b.width * 0.25f, b.height * 0.2f);
                canvas.line_to(b.width * 0.25f, b.height * 0.8f);
                canvas.line_to(b.width * 0.5f, b.height * 0.8f);
                canvas.line_to(b.width * 0.5f, b.height * 0.2f);
                canvas.line_to(b.width * 0.75f, b.height * 0.2f);
                canvas.line_to(b.width * 0.75f, b.height * 0.8f);
                canvas.line_to(b.width, b.height * 0.8f);
                break;
            case PreviewShape::triangle:
                canvas.move_to(0.0f, b.height * 0.5f);
                canvas.line_to(b.width * 0.125f, b.height * 0.15f);
                canvas.line_to(b.width * 0.375f, b.height * 0.85f);
                canvas.line_to(b.width * 0.625f, b.height * 0.15f);
                canvas.line_to(b.width * 0.875f, b.height * 0.85f);
                canvas.line_to(b.width, b.height * 0.5f);
                break;
            case PreviewShape::none:
                break;
        }
        canvas.stroke_current_path();
        return;
    }

    if (samples_.empty()) return;

    // Center line
    auto center_color = resolve_color("waveform.grid", canvas::Color::rgba8(50, 50, 60));
    canvas.set_stroke_color(center_color);
    canvas.set_line_width(0.5f);
    float cy = b.height * 0.5f;
    canvas.stroke_line(0, cy, b.width, cy);

    // Mirrored bar waveform — one vertical bar per column at the column's peak
    // amplitude, mirrored about the centre line (the classic audio-editor
    // thumbnail). Drawn with fill_rect so it renders on the CPU raster path,
    // not only the GPU waveform shader.
    const int cols = std::max(1, std::min<int>(static_cast<int>(b.width), 512));
    const float colw = b.width / static_cast<float>(cols);
    const std::size_t n = samples_.size();
    const float half_h = b.height * 0.5f - 1.0f;
    canvas.set_fill_color(wave_color);
    for (int c = 0; c < cols; ++c) {
        std::size_t i0 = static_cast<std::size_t>(static_cast<float>(c) / cols * n);
        std::size_t i1 = static_cast<std::size_t>(static_cast<float>(c + 1) / cols * n);
        float amp = 0.0f;
        for (std::size_t i = i0; i < i1 && i < n; ++i)
            amp = std::max(amp, std::abs(samples_[i]));
        float half = std::clamp(amp, 0.0f, 1.0f) * half_h;
        float bw = std::max(1.0f, colw - 1.0f);
        canvas.fill_rect(static_cast<float>(c) * colw, cy - half, bw, half * 2.0f + 1.0f);
    }
}

// ── SpectrumView ─────────────────────────────────────────────────────────────

void SpectrumView::set_spectrum(const float* magnitudes_db, size_t bin_count) {
    bins_.assign(magnitudes_db, magnitudes_db + bin_count);
}

void SpectrumView::set_spectrum(std::vector<float> magnitudes_db) {
    bins_ = std::move(magnitudes_db);
}

void SpectrumView::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Background
    auto bg = resolve_color("bg.surface", canvas::Color::rgba8(25, 25, 35));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 2.0f);

    if (bins_.empty()) return;

    float db_range = max_db_ - min_db_;
    if (db_range <= 0) return;

    auto spectrum_color = resolve_color("waveform.line", canvas::Color::rgba8(100, 180, 250));

    if (style_ == Style::bars) {
        canvas.set_fill_color(spectrum_color);
        float bar_width = b.width / static_cast<float>(bins_.size());

        for (size_t i = 0; i < bins_.size(); ++i) {
            float norm = std::clamp((bins_[i] - min_db_) / db_range, 0.0f, 1.0f);
            float bar_h = norm * b.height;
            float x = i * bar_width;
            canvas.fill_rect(x, b.height - bar_h, std::max(bar_width - 1, 1.0f), bar_h);
        }
    } else {
        // Line / filled style — drawn with raster primitives (vertical strips
        // for the area + a polyline on top) so the area fill renders on the CPU
        // CoreGraphics raster path, not only the GPU waveform shader.
        auto fill = resolve_color("waveform.fill", canvas::Color::rgba(spectrum_color.r, spectrum_color.g, spectrum_color.b, 60.0f/255.0f));
        const size_t n = bins_.size();
        auto y_at = [&](size_t i) {
            float norm = std::clamp((bins_[i] - min_db_) / db_range, 0.0f, 1.0f);
            return b.height - norm * b.height;  // 0 dB → top
        };
        const float step = n > 1 ? b.width / static_cast<float>(n - 1) : b.width;

        if (style_ == Style::filled) {
            canvas.set_fill_color(fill);
            for (size_t i = 0; i + 1 < n; ++i) {
                float x0 = static_cast<float>(i) * step;
                // Average the two endpoints so the strip top tracks the curve.
                float y = (y_at(i) + y_at(i + 1)) * 0.5f;
                canvas.fill_rect(x0, y, step + 1.0f, b.height - y);
            }
        }
        // Curve line on top.
        canvas.set_stroke_color(spectrum_color);
        canvas.set_line_width(1.5f);
        for (size_t i = 0; i + 1 < n; ++i) {
            canvas.stroke_line(static_cast<float>(i) * step, y_at(i),
                               static_cast<float>(i + 1) * step, y_at(i + 1));
        }
    }

    // Frequency grid lines (approximate positions)
    auto grid = resolve_color("waveform.grid", canvas::Color::rgba8(50, 50, 65));
    canvas.set_stroke_color(grid);
    canvas.set_line_width(0.5f);

    // Horizontal dB grid lines at -20, -40, -60
    for (float db : {-20.0f, -40.0f, -60.0f}) {
        float norm = std::clamp((db - min_db_) / db_range, 0.0f, 1.0f);
        float y = b.height - norm * b.height;
        canvas.stroke_line(0, y, b.width, y);
    }
}

// ── Panel ────────────────────────────────────────────────────────────────────

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

void SpectrogramView::configure(int history_columns, int freq_rows,
                                 signal::ColorRamp ramp, float min_db, float max_db) {
    buffer_.configure(history_columns, freq_rows);
    mapper_.set_ramp(ramp);
    min_db_ = min_db;
    max_db_ = max_db;
    configured_ = true;
}

void SpectrogramView::push_spectrum(const float* magnitudes_db, int num_bins) {
    if (!configured_) {
        // Auto-configure with sensible defaults
        configure(256, num_bins);
    }
    buffer_.push_column(magnitudes_db, num_bins, mapper_, min_db_, max_db_);
}

void SpectrogramView::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Background
    auto bg = resolve_color("bg.surface", canvas::Color::rgba8(10, 10, 15));
    canvas.set_fill_color(bg);
    canvas.fill_rect(0, 0, b.width, b.height);

    if (!configured_ || buffer_.frames_written() == 0) return;

    int buf_w = buffer_.width();
    int buf_h = buffer_.height();
    float px_w = b.width / buf_w;
    float px_h = b.height / buf_h;

    int start_col = buffer_.write_column(); // oldest column

    for (int col = 0; col < buf_w; ++col) {
        int src_col = (start_col + col) % buf_w;
        float x = col * px_w;

        for (int row = 0; row < buf_h; ++row) {
            auto c = buffer_.pixels()[row * buf_w + src_col];
            // Flip vertically: low freq at bottom
            float y = b.height - (row + 1) * px_h;
            canvas.set_fill_color(canvas::Color::rgba(c.r, c.g, c.b, c.a));
            canvas.fill_rect(x, y, px_w + 0.5f, px_h + 0.5f);
        }
    }
}

// ── MultiMeter ──────────────────────────────────────────────────────────────

void MultiMeter::set_channel_count(int count) {
    ballistics_.num_channels = std::min(count, signal::kMaxMeterChannels);
}

void MultiMeter::update(const signal::MultiChannelMeterData& data, float dt) {
    ballistics_.update(data, dt);
}

void MultiMeter::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    int num_ch = ballistics_.num_channels;
    if (num_ch <= 0) return;

    if (display_style_ == DisplayStyle::segmented && layout_ == Layout::horizontal) {
        constexpr float kMinDb = -60.0f;
        constexpr float kMaxDb = 6.0f;
        auto level_to_position = [&](float level) {
            if (level <= 0.0f) return 0.0f;
            const float db = 20.0f * std::log10(std::max(level, 1.0e-6f));
            return std::clamp((db - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);
        };
        auto position_to_db = [&](float position) {
            return kMinDb + std::clamp(position, 0.0f, 1.0f) * (kMaxDb - kMinDb);
        };
        auto active_color_for_db = [](float db) {
            if (db >= 0.0f) return canvas::Color::rgba8(255, 48, 48);
            if (db >= -6.0f) return canvas::Color::rgba8(246, 218, 61);
            return canvas::Color::rgba8(134, 232, 68);
        };
        auto inactive_color_for_db = [](float db) {
            if (db >= 0.0f) return canvas::Color::rgba8(66, 16, 18);
            if (db >= -6.0f) return canvas::Color::rgba8(62, 48, 17);
            return canvas::Color::rgba8(23, 43, 18);
        };
        auto label_for_channel = [](int ch) -> const char* {
            if (ch == 0) return "L";
            if (ch == 1) return "R";
            if (ch == 2) return "C";
            if (ch == 3) return "Ls";
            if (ch == 4) return "Rs";
            return "";
        };

        canvas.set_fill_color(resolve_color("bg.primary", canvas::Color::rgba8(7, 8, 11)));
        canvas.fill_rounded_rect(0, 0, b.width, b.height, 3.0f);

        const float pad = 4.0f;
        const float label_w = 18.0f;
        const float scale_h = b.height >= 36.0f ? 12.0f : 0.0f;
        const float meter_x = label_w + pad;
        const float meter_w = std::max(1.0f, b.width - meter_x - pad);
        const float row_gap = 3.0f;
        const float row_area_h = std::max(
            1.0f,
            b.height - pad * 2.0f - scale_h - row_gap * static_cast<float>(num_ch - 1));
        const float row_h = std::max(3.0f, row_area_h / static_cast<float>(num_ch));
        const int segments = meter_w >= 180.0f ? 36 : 24;
        const float segment_gap = 2.0f;
        const float segment_w = std::max(
            2.0f,
            (meter_w - segment_gap * static_cast<float>(segments - 1)) /
                static_cast<float>(segments));

        canvas.set_font("Inter", 9.0f);
        for (int ch = 0; ch < num_ch; ++ch) {
            const auto& bc = ballistics_.channels[ch];
            const float y0 = pad + static_cast<float>(ch) * (row_h + row_gap);
            const float y_mid = y0 + row_h * 0.5f;
            const float peak_pos = level_to_position(bc.display_peak);
            const float rms_pos = level_to_position(bc.display_rms);

            canvas.set_fill_color(resolve_color("accent.success", canvas::Color::rgba8(134, 232, 68)));
            const char* label = label_for_channel(ch);
            if (label[0] != '\0') canvas.fill_text(label, pad, y_mid + 3.0f);

            for (int i = 0; i < segments; ++i) {
                const float segment_pos =
                    (static_cast<float>(i) + 0.5f) / static_cast<float>(segments);
                const float db = position_to_db(segment_pos);
                const bool active = segment_pos <= peak_pos;
                auto color = active ? active_color_for_db(db) : inactive_color_for_db(db);
                if (active && segment_pos > rms_pos) color.a = 175;
                canvas.set_fill_color(color);
                const float x = meter_x + static_cast<float>(i) * (segment_w + segment_gap);
                canvas.fill_rect(x, y0, segment_w, row_h);
            }

            const float held_pos = level_to_position(bc.held_peak);
            if (held_pos > 0.0f) {
                canvas.set_stroke_color(bc.held_peak >= 1.0f
                    ? resolve_color("accent.error", canvas::Color::rgba8(255, 48, 48))
                    : canvas::Color::rgba8(255, 255, 255));
                canvas.set_line_width(1.0f);
                const float x = meter_x + held_pos * meter_w;
                canvas.stroke_line(x, y0 - 1.0f, x, y0 + row_h + 1.0f);
            }

            if (bc.clip_indicator) {
                canvas.set_fill_color(resolve_color("accent.error", canvas::Color::rgba8(255, 48, 48)));
                canvas.fill_rect(b.width - pad - 3.0f, y0, 3.0f, row_h);
            }
        }

        if (scale_h > 0.0f) {
            struct Tick { float db; const char* label; };
            constexpr Tick ticks[] = {
                {-20.0f, "-20"}, {-15.0f, "-15"}, {-10.0f, "-10"},
                { -6.0f,  "-6"}, { -3.0f,  "-3"}, {  0.0f,   "0"},
                {  2.0f,  "+2"}, {  4.0f,  "+4"},
            };
            canvas.set_font("Inter", 8.0f);
            canvas.set_fill_color(resolve_color("text.secondary", canvas::Color::rgba8(160, 166, 185)));
            for (const auto& tick : ticks) {
                const float pos = std::clamp((tick.db - kMinDb) / (kMaxDb - kMinDb), 0.0f, 1.0f);
                const float x = meter_x + pos * meter_w;
                canvas.set_stroke_color(canvas::Color::rgba8(58, 64, 73));
                canvas.set_line_width(1.0f);
                canvas.stroke_line(x, pad, x, b.height - scale_h - 1.0f);
                canvas.set_fill_color(resolve_color("text.secondary", canvas::Color::rgba8(160, 166, 185)));
                canvas.fill_text(tick.label, x - 6.0f, b.height - 3.0f);
            }
            canvas.fill_text("dB", meter_x - 14.0f, b.height - 3.0f);
        }
        return;
    }

    // Background
    auto bg = resolve_color("control.track", canvas::Color::rgba8(30, 30, 30));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 2.0f);

    bool vert = (layout_ == Layout::vertical);
    float gap = 2.0f;

    // Calculate per-channel dimensions
    float ch_size;
    if (vert)
        ch_size = (b.width - gap * (num_ch - 1)) / num_ch;
    else
        ch_size = (b.height - gap * (num_ch - 1)) / num_ch;

    for (int ch = 0; ch < num_ch; ++ch) {
        auto& bc = ballistics_.channels[ch];
        float meter_length = vert ? b.height : b.width;

        // Channel position
        float x0, y0, cw, ch_h;
        if (vert) {
            x0 = ch * (ch_size + gap);
            y0 = 0;
            cw = ch_size;
            ch_h = b.height;
        } else {
            x0 = 0;
            y0 = ch * (ch_size + gap);
            cw = b.width;
            ch_h = ch_size;
        }

        // RMS fill
        auto rms_color = resolve_color("accent.success", canvas::Color::rgba8(80, 200, 80));
        float rms_level = bc.display_rms;
        if (rms_level > 0.9f)
            rms_color = resolve_color("accent.error", canvas::Color::rgba8(240, 60, 60));
        else if (rms_level > 0.7f)
            rms_color = resolve_color("accent.warning", canvas::Color::rgba8(240, 180, 60));

        canvas.set_fill_color(rms_color);
        float fill = rms_level * meter_length;
        if (vert)
            canvas.fill_rect(x0, ch_h - fill, cw, fill);
        else
            canvas.fill_rect(x0, y0, fill, ch_h);

        // Peak indicator
        float peak_level = bc.display_peak;
        if (peak_level > 0.01f) {
            canvas.set_stroke_color(canvas::Color::rgba8(255, 255, 255));
            canvas.set_line_width(1.0f);
            float peak_pos = peak_level * meter_length;
            if (vert) {
                float y = ch_h - peak_pos;
                canvas.stroke_line(x0, y, x0 + cw, y);
            } else {
                canvas.stroke_line(peak_pos, y0, peak_pos, y0 + ch_h);
            }
        }

        // Held peak
        float held = bc.held_peak;
        if (held > 0.01f) {
            auto held_color = held > 0.9f
                ? resolve_color("accent.error", canvas::Color::rgba8(255, 50, 50))
                : canvas::Color::rgba8(255, 100, 100);
            canvas.set_stroke_color(held_color);
            canvas.set_line_width(2.0f);
            float held_pos = held * meter_length;
            if (vert) {
                float y = ch_h - held_pos;
                canvas.stroke_line(x0, y, x0 + cw, y);
            } else {
                canvas.stroke_line(held_pos, y0, held_pos, y0 + ch_h);
            }
        }

        // Clip indicator
        if (bc.clip_indicator) {
            canvas.set_fill_color(resolve_color("accent.error", canvas::Color::rgba8(255, 0, 0)));
            if (vert)
                canvas.fill_rect(x0, 0, cw, 3.0f);
            else
                canvas.fill_rect(cw - 3.0f, y0, 3.0f, ch_h);
        }
    }
}

// ── CorrelationMeter ────────────────────────────────────────────────────────

void CorrelationMeter::update(float correlation, float dt) {
    float target = std::clamp(correlation, -1.0f, 1.0f);
    float coeff = 1.0f - std::exp(-dt / 0.05f); // 50ms smoothing
    display_correlation_ += (target - display_correlation_) * coeff;
}

void CorrelationMeter::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Background
    auto bg = resolve_color("control.track", canvas::Color::rgba8(30, 30, 30));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 2.0f);

    // Center line (0.0 correlation)
    float center_x = b.width * 0.5f;
    canvas.set_stroke_color(canvas::Color::rgba8(80, 80, 80));
    canvas.set_line_width(1.0f);
    canvas.stroke_line(center_x, 0, center_x, b.height);

    // -1 and +1 labels position markers
    float quarter = b.width * 0.25f;
    canvas.set_stroke_color(canvas::Color::rgba8(50, 50, 50));
    canvas.stroke_line(quarter, 0, quarter, b.height);
    canvas.stroke_line(b.width - quarter, 0, b.width - quarter, b.height);

    // Correlation indicator
    // Map -1..+1 to 0..width, inset by half bar width to prevent edge clipping
    float norm = (display_correlation_ + 1.0f) * 0.5f; // 0..1
    float bar_width = std::max(4.0f, b.width * 0.02f);
    float usable = b.width - bar_width;
    float indicator_x = bar_width * 0.5f + norm * usable;

    // Color: green at +1, yellow at 0, red at -1
    canvas::Color indicator_color;
    if (display_correlation_ > 0.0f) {
        // Green to yellow
        float t = display_correlation_;
        indicator_color = canvas::Color::rgba8(
            static_cast<uint8_t>(240 * (1.0f - t)),
            static_cast<uint8_t>(200 + 55 * t),
            static_cast<uint8_t>(60 * (1.0f - t)));
    } else {
        // Yellow to red
        float t = -display_correlation_;
        indicator_color = canvas::Color::rgba8(
            static_cast<uint8_t>(240),
            static_cast<uint8_t>(200 * (1.0f - t)),
            static_cast<uint8_t>(60 * (1.0f - t)));
    }

    // Draw indicator bar
    canvas.set_fill_color(indicator_color);
    canvas.fill_rounded_rect(indicator_x - bar_width * 0.5f, 1,
                              bar_width, b.height - 2, 2.0f);

    // Fill from center to indicator
    auto fill_color = indicator_color;
    fill_color.a = 80;
    canvas.set_fill_color(fill_color);
    if (indicator_x > center_x) {
        canvas.fill_rect(center_x, 2, indicator_x - center_x, b.height - 4);
    } else {
        canvas.fill_rect(indicator_x, 2, center_x - indicator_x, b.height - 4);
    }
}


} // namespace pulp::view
