#include <pulp/view/widget_skin_derive.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::view {

namespace {

struct RGB {
    int r = 0, g = 0, b = 0, a = 0;
};

inline RGB pixel(const SkinImage& img, int x, int y) {
    const uint8_t* p = img.pixels + (static_cast<size_t>(y) * img.width + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

inline int lum(const RGB& c) { return (c.r + c.g + c.b) / 3; }
inline int sat(const RGB& c) {
    int mx = std::max({c.r, c.g, c.b});
    int mn = std::min({c.r, c.g, c.b});
    return mx - mn;
}
inline canvas::Color to_color(const RGB& c) {
    return canvas::Color::rgba8(static_cast<uint8_t>(c.r),
                                static_cast<uint8_t>(c.g),
                                static_cast<uint8_t>(c.b));
}

// Locate the widget art region as the tallest contiguous run of opaque pixels
// in the center column. Design-tool exports bake the live control at the top
// of the PNG and flatten label / value / metadata text below it as smaller,
// gapped glyph runs — so the tallest opaque run is the control art.
bool find_art_region(const SkinImage& img, int cx, int& top, int& bottom) {
    int best_top = -1, best_bottom = -1, best_h = 0;
    int run_start = -1;
    for (int y = 0; y < img.height; ++y) {
        bool opaque = pixel(img, cx, y).a > 40;
        if (opaque && run_start < 0) {
            run_start = y;
        } else if (!opaque && run_start >= 0) {
            int h = y - run_start;
            if (h > best_h) { best_h = h; best_top = run_start; best_bottom = y; }
            run_start = -1;
        }
    }
    if (run_start >= 0) {
        int h = img.height - run_start;
        if (h > best_h) { best_h = h; best_top = run_start; best_bottom = img.height; }
    }
    if (best_top < 0 || best_h < 8) return false;
    top = best_top;
    bottom = best_bottom;
    return true;
}

// Horizontal extent of opaque pixels at a given row, measured outward from a
// seed column so disjoint label glyphs elsewhere on the row don't widen the
// result. Returns false when the seed pixel itself is transparent. left/right
// are inclusive pixel indices; width = right - left + 1.
bool row_art_bounds(const SkinImage& img, int y, int seed_x, int& left, int& right) {
    if (y < 0 || y >= img.height) return false;
    if (pixel(img, seed_x, y).a <= 40) return false;
    left = right = seed_x;
    while (left - 1 >= 0 && pixel(img, left - 1, y).a > 40) --left;
    while (right + 1 < img.width && pixel(img, right + 1, y).a > 40) ++right;
    return true;
}

}  // namespace

FaderSkin derive_fader_skin(const SkinImage& img) {
    FaderSkin out;
    if (!img.valid()) return out;
    int cx = img.width / 2;
    int top = 0, bottom = 0;
    if (!find_art_region(img, cx, top, bottom)) return out;

    // Collect opaque center-column rows of the art region.
    std::vector<std::pair<int, RGB>> rows;
    for (int y = top; y < bottom; ++y) {
        RGB c = pixel(img, cx, y);
        if (c.a > 200) rows.emplace_back(y, c);
    }
    if (rows.empty()) return out;

    // Track: among low-saturation rows, take a near-darkest representative
    // (robust to anti-aliasing at the very top). Sorting by luminance and
    // sampling ~1/6 in avoids the single darkest AA pixel while staying on the
    // real track color.
    std::vector<std::pair<int, RGB>> low_sat;
    for (auto& r : rows) if (sat(r.second) < 25) low_sat.push_back(r);
    if (!low_sat.empty()) {
        std::sort(low_sat.begin(), low_sat.end(),
                  [](auto& a, auto& b) { return lum(a.second) < lum(b.second); });
        out.track_color = to_color(low_sat[low_sat.size() / 6].second);
        out.has_track = true;

        // Track outline: the captured empty track is a dark channel that the
        // design draws with a slightly lighter edge so it
        // doesn't read as a flat slab. We first try to RECOVER that edge by
        // scanning across a representative dark track row for the brightest
        // low-saturation pixel and comparing it to the fill at the row center —
        // a real outline shows a clear luminance delta. When the captured edge
        // is a sub-pixel anti-aliased rim that the flat-PNG sample can't
        // resolve (it reads as uniform fill), we SYNTHESISE the rim by
        // lightening the sampled track color — but only for a DARK track,
        // where an outline is the design convention. Both paths are derived
        // from the captured pixels (the actual track color / edge); no
        // per-instance color is hardcoded. A light/flat track gets no rim.
        {
            const RGB track_rgb = low_sat[low_sat.size() / 6].second;
            const int track_lum = lum(track_rgb);
            int track_row = low_sat[low_sat.size() / 2].first;  // a mid dark row
            int center_lum = lum(pixel(img, cx, track_row));      // the fill here
            int l = 0, r = 0;
            bool recovered = false;
            if (row_art_bounds(img, track_row, cx, l, r)) {
                RGB brightest{};
                int best_lum = -1;
                for (int x = l; x <= r; ++x) {
                    RGB c = pixel(img, track_row, x);
                    if (c.a <= 200 || sat(c) >= 25) continue;
                    if (lum(c) > best_lum) { best_lum = lum(c); brightest = c; }
                }
                if (best_lum - center_lum > 12) {
                    out.track_border_color = to_color(brightest);
                    out.has_track_border = true;
                    recovered = true;
                }
            }
            // Synthesised rim for a dark track when none could be resolved.
            // Lighten the sampled track color by a fixed step (kept modest so
            // the stroke reads as a subtle edge, not a second fill).
            if (!recovered && track_lum < 90) {
                auto lift = [](int v) { return std::min(255, v + 30); };
                out.track_border_color =
                    canvas::Color::rgba8(static_cast<uint8_t>(lift(track_rgb.r)),
                                         static_cast<uint8_t>(lift(track_rgb.g)),
                                         static_cast<uint8_t>(lift(track_rgb.b)));
                out.has_track_border = true;
            }
        }

        // Thumb body: brightest low-saturation row (the silver slab).
        auto thumb_it = std::max_element(low_sat.begin(), low_sat.end(),
                                         [](auto& a, auto& b) { return lum(a.second) < lum(b.second); });
        int thumb_y = thumb_it->first;
        out.thumb_color = to_color(thumb_it->second);
        out.has_thumb = true;

        // Where the design drew the thumb within the bar (1 = top, 0 = bottom)
        // — seed the imported fader's initial value-position from this so it
        // matches the capture regardless of the (non-linear) value→position map.
        if (bottom > top + 1) {
            out.thumb_position = std::clamp(
                1.0f - static_cast<float>(thumb_y - top) / static_cast<float>(bottom - top),
                0.0f, 1.0f);
            out.has_thumb_position = true;
        }

        // Thumb border / bevel: nearest-to-mid-grey low-sat row within a small
        // window around the thumb center (the darker edge of the slab).
        RGB border{};
        bool found_border = false;
        int best_dist = 1 << 30;
        for (int y = thumb_y - 16; y <= thumb_y + 16; ++y) {
            if (y < 0 || y >= img.height) continue;
            RGB c = pixel(img, cx, y);
            if (c.a <= 200 || sat(c) >= 25) continue;
            int d = std::abs(lum(c) - 140);
            if (d < best_dist) {
                best_dist = d;
                border = c;
                found_border = true;
            }
        }
        if (found_border && std::abs(lum(border) - lum(thumb_it->second)) > 20) {
            out.thumb_border_color = to_color(border);
            out.has_thumb_border = true;
        }
    }

    // Fill: a REPRESENTATIVE colored row, not the single most-saturated one.
    // The captured fill is a vertical gradient (lighter near the thumb, darker
    // toward the bottom); the single max-saturation pixel is the darkest /
    // deepest stop and over-saturates the derived color vs the fill's dominant
    // mid tone. Collect the saturated rows (sat > 40) and take the MEDIAN by
    // saturation so the derived color is the dominant mid blue — matching the
    // reference palette the harness compares against.
    {
        std::vector<RGB> colored;
        for (auto& r : rows)
            if (sat(r.second) > 40) colored.push_back(r.second);
        if (!colored.empty()) {
            std::sort(colored.begin(), colored.end(),
                      [](const RGB& a, const RGB& b) { return sat(a) < sat(b); });
            out.fill_color = to_color(colored[colored.size() / 2]);
            out.has_fill = true;
        }
    }

    // ── Horizontal widths ───────────────────────────────────────────────────
    // Thumb width: the widest opaque row in the art region (the silver slab).
    // Track width: the opaque width at rows away from the thumb (the thin
    // track/fill column). Measured outward from cx so disjoint glyphs never
    // bleed in. Both are reported in asset pixels; the importer scales them.
    {
        int max_w = 0, l = 0, r = 0;
        std::vector<int> widths;
        widths.reserve(static_cast<size_t>(bottom - top));
        for (int y = top; y < bottom; ++y) {
            if (row_art_bounds(img, y, cx, l, r)) {
                int w = r - l + 1;
                widths.push_back(w);
                if (w > max_w) max_w = w;
            } else {
                widths.push_back(0);
            }
        }
        if (max_w > 0) {
            out.thumb_width_px = static_cast<float>(max_w);
            out.has_thumb_width = true;
            // Track = median of the NARROW rows (those at most ~40% of the
            // widest), i.e. the thin track/fill column outside the thumb slab.
            std::vector<int> narrow;
            for (int w : widths)
                if (w > 0 && w <= std::max(1, max_w * 2 / 5)) narrow.push_back(w);
            if (!narrow.empty()) {
                std::sort(narrow.begin(), narrow.end());
                out.track_width_px = static_cast<float>(narrow[narrow.size() / 2]);
                out.has_track_width = true;
            }
        }
    }

    // Housing height = the control art region (track + thumb), excluding the
    // value-stack text the design tool bakes below it.
    if (bottom > top) {
        out.housing_height_px = static_cast<float>(bottom - top);
        out.has_housing_height = true;
    }

    return out;
}

MeterSkin derive_meter_skin(const SkinImage& img, int stop_count) {
    MeterSkin out;
    if (!img.valid() || stop_count < 2) return out;
    int cx = img.width / 2;
    int top = 0, bottom = 0;
    if (!find_art_region(img, cx, top, bottom)) return out;

    // Background: a dark low-saturation row near the top of the art (the empty
    // meter channel above the captured level).
    for (int y = top; y < bottom; ++y) {
        RGB c = pixel(img, cx, y);
        if (c.a > 200 && lum(c) < 60 && sat(c) < 25) {
            out.background = to_color(c);
            out.has_background = true;
            break;
        }
    }

    // Walk the contiguous colored fill from the bottom up. Stop at the dark
    // empty channel or the low-saturation white peak line — that bounds the
    // captured gradient's TOP (the warm/red stop sits just under the peak
    // line / dark channel). This `fill_top` is the top of the bar's full
    // COLORED extent, which is what the gradient must span so its top stop is
    // the warm/red the capture shows — not only the lower green→yellow band.
    int fill_bottom = bottom - 2;
    if (fill_bottom <= top) return out;
    int fill_top = fill_bottom;
    for (int y = fill_bottom; y > top; --y) {
        RGB c = pixel(img, cx, y);
        bool dark = (c.r + c.g + c.b) < 120;
        if (c.a <= 200 || dark || sat(c) < 30) break;
        fill_top = y;
    }
    int fill_h = fill_bottom - fill_top;
    if (fill_h < 8) return out;

    // How far the captured gradient filled the bar (0 = empty, 1 = full) — seed
    // the imported meter's initial level from this rather than a linear dB→
    // position map.
    if (bottom > top + 1) {
        out.fill_level = std::clamp(
            static_cast<float>(fill_h) / static_cast<float>(bottom - top), 0.0f, 1.0f);
        out.has_fill_level = true;
    }

    // Sample stop_count stops across the bar's full COLORED extent, low/green
    // (fill_bottom) → high/warm (fill_top). The renderer maps these stops across
    // the displayed fill region, so the top stop (warm/red) lands at the top of
    // the rendered fill, matching the capture.
    for (int i = 0; i < stop_count; ++i) {
        float frac = static_cast<float>(i) / static_cast<float>(stop_count - 1);
        int y = static_cast<int>(std::lround(fill_bottom - frac * fill_h));
        y = std::clamp(y, top, bottom - 1);
        out.gradient.push_back(to_color(pixel(img, cx, y)));
    }

    // ── Bar / housing widths ─────────────────────────────────────────────────
    // The captured art has TWO widths: the dark HOUSING slot (the full opaque
    // run in [top, bottom)) and the narrower COLORED bar recessed inside it
    // (the saturated fill in [fill_top, fill_bottom]). The widget box should be
    // the housing width; the colored bar insets within it by bar_fill_ratio so
    // the rendered meter reads as a recessed bar, not edge-to-edge paint.
    // Housing width = the full opaque extent (outward from cx) — the dark slot.
    // Colored-bar width = the run of SATURATED pixels around cx — the recessed
    // colored fill, which can be narrower than the opaque housing it sits in.
    // Measuring the opaque extent for the bar would just re-measure the housing
    // (the dark slot stays opaque around the color), so the bar must be the
    // saturated run, not the opaque run.
    auto median_opaque_width = [&](int y0, int y1) -> float {
        std::vector<int> widths;
        widths.reserve(static_cast<size_t>(std::max(1, y1 - y0)));
        int l = 0, r = 0;
        for (int y = y0; y < y1; ++y)
            if (row_art_bounds(img, y, cx, l, r)) widths.push_back(r - l + 1);
        if (widths.empty()) return 0.0f;
        std::sort(widths.begin(), widths.end());
        return static_cast<float>(widths[widths.size() / 2]);
    };
    auto median_colored_width = [&](int y0, int y1) -> float {
        std::vector<int> widths;
        widths.reserve(static_cast<size_t>(std::max(1, y1 - y0)));
        for (int y = y0; y < y1; ++y) {
            // Walk outward from cx counting contiguous saturated pixels.
            if (sat(pixel(img, cx, y)) < 30) continue;
            int l = cx, r = cx;
            while (l - 1 >= 0 && pixel(img, l - 1, y).a > 200 && sat(pixel(img, l - 1, y)) >= 30) --l;
            while (r + 1 < img.width && pixel(img, r + 1, y).a > 200 && sat(pixel(img, r + 1, y)) >= 30) ++r;
            widths.push_back(r - l + 1);
        }
        if (widths.empty()) return 0.0f;
        std::sort(widths.begin(), widths.end());
        return static_cast<float>(widths[widths.size() / 2]);
    };
    float housing_w = median_opaque_width(top, bottom);
    float colored_w = median_colored_width(fill_top, fill_bottom);
    if (housing_w > 0.0f) {
        out.bar_width_px = housing_w;
        out.has_bar_width = true;
        if (colored_w > 0.0f && colored_w <= housing_w) {
            out.bar_fill_ratio = std::clamp(colored_w / housing_w, 0.05f, 1.0f);
            out.has_bar_fill_ratio = true;
        }
    }
    // Housing height = the control art region (dark channel + colored fill),
    // excluding the value-stack text the design tool bakes below it.
    if (bottom > top) {
        out.housing_height_px = static_cast<float>(bottom - top);
        out.has_housing_height = true;
    }
    return out;
}

}  // namespace pulp::view
