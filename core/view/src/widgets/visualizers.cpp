// widgets/<name>.cpp — extracted from core/view/src/widgets.cpp in the
// 2026-05 Phase 2 (R2-6) batch. The 2,081-line widgets monolith was
// adding 20+ touches/60 days and producing constant cross-widget merge
// conflicts; splitting per major widget gives each its own conflict
// surface.
//
// Per Codex's R2-6 risk callout, no header changes — every public
// declaration stays in core/view/include/pulp/view/widgets.hpp.

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

    // Workstream 07 B4: when an ImageCache is attached, consult it.
    // The cache owns decode + eviction; the view just renders whatever
    // the cache hands back. A null DecodedImage means decode-in-progress
    // or permanent failure, in which case we fall through to the
    // path-as-text placeholder so the UI never shows a blank rect.
    if (cache_) {
        const auto* decoded = cache_->get(path_);
        if (decoded && decoded->native_handle) {
            // Hand the opaque native handle to the canvas. Backends that
            // know how to interpret it (CoreGraphics → CGImageRef,
            // Skia → SkImage*) blit it; others no-op and we fall through
            // to the filename placeholder. Workstream 07 slice B4/#255.
            canvas.draw_image(decoded->native_handle, 0, 0, b.width, b.height);
            return;
        }
    }

    // pulp #1658 — decode-on-paint via the canvas's draw_image_from_file
    // primitive. Skia's implementation uses SkData::MakeFromFileName +
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

    // pulp #1737 — honour CSS `object-fit` + `object-position`.
    // Probe intrinsic image dimensions; if the backend can't measure
    // (no decode primitive on this platform) fall back to the
    // pre-#1737 stretch-to-bounds path (= object-fit: fill).
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
        // Same applies when `object-fit` is unset or unknown — the
        // pre-#1737 behaviour.
        if (fit.empty() || fit == "fill") {
            return {dst, src};
        }

        // `none` — natural size, no scaling. Crop or letterbox both axes.
        // Centre by default; refined below by object-position parsing.
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

        // pulp #1737 — apply `object-position` as a percentage offset.
        // CSS spec lets the value be lengths or percentages; the JS
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

        // Route via the source-rect overload when `none` / `cover` / a
        // non-identity src was computed. The dst-only path stays for
        // the simple `fill` / `contain` cases (full src rect).
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
        if (drawn) { loaded_ = true; return; }
    } else if (!fs_path.empty() &&
               canvas.draw_image_from_file(fs_path, 0, 0, b.width, b.height)) {
        loaded_ = true;
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

void Meter::set_level(float rms, float peak) {
    current_rms_ = std::clamp(rms, 0.0f, 1.0f);
    current_peak_ = std::clamp(peak, 0.0f, 1.0f);
    ballistics_.display_rms = current_rms_;
    ballistics_.display_peak = current_peak_;
    request_repaint();
}

void Meter::update(float raw_peak, float raw_rms, float dt) {
    ballistics_.update(raw_peak, raw_rms, dt);
    request_repaint();
}

void Meter::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    bool vert = orientation_ == Orientation::vertical;

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
        // Round corners by clipping (approximate with rounded rect overlay)
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

    // RMS fill (main body)
    auto rms_color = resolve_color("accent.success", canvas::Color::rgba8(80, 200, 80));
    float rms_level = ballistics_.display_rms;

    // Color changes at different levels
    if (rms_level > 0.9f)
        rms_color = resolve_color("accent.error", canvas::Color::rgba8(240, 60, 60));
    else if (rms_level > 0.7f)
        rms_color = resolve_color("accent.warning", canvas::Color::rgba8(240, 180, 60));

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

    // Grid lines
    auto grid = resolve_color("control.border", canvas::Color::rgba8(60, 60, 75));
    canvas.set_stroke_color(grid);
    canvas.set_line_width(0.5f);
    canvas.stroke_line(b.width * 0.5f, 0, b.width * 0.5f, b.height);
    canvas.stroke_line(0, b.height * 0.5f, b.width, b.height * 0.5f);

    // Crosshair position — inset by dot radius so it doesn't clip at edges
    float dot_r = 5.0f;
    float cx = dot_r + x_ * (b.width - 2.0f * dot_r);
    float cy = dot_r + (1.0f - y_) * (b.height - 2.0f * dot_r);

    // Crosshair lines
    auto hair_color = resolve_color("control.fill", canvas::Color::rgba8(100, 150, 255));
    canvas.set_stroke_color(hair_color);
    canvas.set_line_width(1.0f);
    canvas.stroke_line(cx, 0, cx, b.height);
    canvas.stroke_line(0, cy, b.width, cy);

    // Thumb dot
    auto thumb = resolve_color("control.thumb", canvas::Color::rgba8(220, 220, 220));
    canvas.set_fill_color(thumb);
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
    thumbnail_ = nullptr;  // raw samples win over any cached thumbnail
}

void WaveformView::set_data(std::vector<float> samples) {
    samples_ = std::move(samples);
    apply_trigger();
    thumbnail_ = nullptr;
}

void WaveformView::set_thumbnail(const pulp::audio::AudioThumbnail* thumb,
                                  uint32_t channel) {
    thumbnail_ = thumb;
    thumbnail_channel_ = channel;
    // A live thumbnail shadows any prior raw sample buffer at paint time;
    // we drop the samples cache so memory doesn't double up.
    samples_.clear();
}

void WaveformView::clear_thumbnail() {
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

    // Thumbnail path (Pulp #2843 / item 6.12): render decimated min/max
    // peaks straight from the cached thumbnail without re-decoding audio.
    if (thumbnail_ != nullptr && !thumbnail_->empty()) {
        // Center line
        auto center_color = resolve_color("waveform.grid", canvas::Color::rgba8(50, 50, 60));
        canvas.set_stroke_color(center_color);
        canvas.set_line_width(0.5f);
        float cy = b.height * 0.5f;
        canvas.stroke_line(0, cy, b.width, cy);

        // Render one (min, max) pair per pixel column. ~2 KB on the stack
        // for a 1024-wide widget; cap at 4096 px to stay bounded.
        const std::size_t target_peaks =
            std::min<std::size_t>(static_cast<std::size_t>(std::max(1.0f, b.width)), 4096u);
        std::vector<float> min_max(target_peaks * 2, 0.0f);
        const std::size_t produced = thumbnail_->render_min_max(
            thumbnail_channel_, static_cast<uint32_t>(target_peaks), min_max.data());
        if (produced == 0) return;

        // Drive the GPU waveform shader with the per-column max so the
        // line/fill remain consistent with the live-sample path. We render
        // the absolute envelope (max(|min|, |max|)) — perceptually closest
        // to traditional audio-editor waveform displays.
        std::vector<float> envelope(produced);
        for (std::size_t i = 0; i < produced; ++i) {
            const float lo = min_max[i * 2 + 0];
            const float hi = min_max[i * 2 + 1];
            const float amp = std::max(std::abs(lo), std::abs(hi));
            // Signed envelope: top half positive, bottom half negative would
            // require two passes through the shader. The shader already
            // handles symmetric ±amp, so we feed it max amplitude per column.
            envelope[i] = hi >= -lo ? amp : -amp;
        }

        canvas::Canvas::WaveformStyle style;
        style.line_color = wave_color;
        style.fill_color = fill_color;
        style.line_thickness = 2.0f;
        style.show_fill = true;
        style.fill_center = 0.5f;

        canvas.draw_waveform(envelope.data(), envelope.size(),
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

    // GPU-accelerated waveform rendering via SkSL shader
    canvas::Canvas::WaveformStyle style;
    style.line_color = wave_color;
    style.fill_color = fill_color;
    style.line_thickness = 2.0f;
    style.show_fill = true;
    style.fill_center = 0.5f;

    canvas.draw_waveform(samples_.data(), samples_.size(), 0, 0, b.width, b.height, style);
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
        // Line or filled style — use GPU waveform shader for anti-aliased rendering
        auto fill = resolve_color("waveform.fill", canvas::Color::rgba(spectrum_color.r, spectrum_color.g, spectrum_color.b, 60.0f/255.0f));

        // Normalize dB values to -1..1 for the waveform shader (0 dB → top, min_db → bottom)
        std::vector<float> normalized(bins_.size());
        for (size_t i = 0; i < bins_.size(); ++i) {
            float norm = std::clamp((bins_[i] - min_db_) / db_range, 0.0f, 1.0f);
            normalized[i] = norm * 2.0f - 1.0f;  // Map 0..1 → -1..1
        }

        canvas::Canvas::WaveformStyle ws;
        ws.line_color = spectrum_color;
        ws.fill_color = fill;
        ws.line_thickness = 1.5f;
        ws.show_fill = (style_ == Style::filled);
        ws.fill_center = 1.0f;  // Fill from bottom

        canvas.draw_waveform(normalized.data(), normalized.size(), 0, 0, b.width, b.height, ws);
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
    // pulp #1663 — resolve the effective uniform radius (px or %).
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
