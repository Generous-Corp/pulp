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
// in the centre column. Design-tool exports bake the live control at the top
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

}  // namespace

FaderSkin derive_fader_skin(const SkinImage& img) {
    FaderSkin out;
    if (!img.valid()) return out;
    int cx = img.width / 2;
    int top = 0, bottom = 0;
    if (!find_art_region(img, cx, top, bottom)) return out;

    // Collect opaque centre-column rows of the art region.
    std::vector<std::pair<int, RGB>> rows;
    for (int y = top; y < bottom; ++y) {
        RGB c = pixel(img, cx, y);
        if (c.a > 200) rows.emplace_back(y, c);
    }
    if (rows.empty()) return out;

    // Track: among low-saturation rows, take a near-darkest representative
    // (robust to anti-aliasing at the very top). Sorting by luminance and
    // sampling ~1/6 in avoids the single darkest AA pixel while staying on the
    // real track colour.
    std::vector<std::pair<int, RGB>> low_sat;
    for (auto& r : rows) if (sat(r.second) < 25) low_sat.push_back(r);
    if (!low_sat.empty()) {
        std::sort(low_sat.begin(), low_sat.end(),
                  [](auto& a, auto& b) { return lum(a.second) < lum(b.second); });
        out.track_color = to_color(low_sat[low_sat.size() / 6].second);
        out.has_track = true;

        // Thumb body: brightest low-saturation row (the silver slab).
        auto thumb_it = std::max_element(low_sat.begin(), low_sat.end(),
                                         [](auto& a, auto& b) { return lum(a.second) < lum(b.second); });
        int thumb_y = thumb_it->first;
        out.thumb_color = to_color(thumb_it->second);
        out.has_thumb = true;

        // Thumb border / bevel: nearest-to-mid-grey low-sat row within a small
        // window around the thumb centre (the darker edge of the slab).
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

    // Fill: the most-saturated row in the art (the coloured track fill).
    auto fill_it = std::max_element(rows.begin(), rows.end(),
                                    [](auto& a, auto& b) { return sat(a.second) < sat(b.second); });
    if (fill_it != rows.end() && sat(fill_it->second) > 40) {
        out.fill_color = to_color(fill_it->second);
        out.has_fill = true;
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

    // Walk the contiguous coloured fill from the bottom up. Stop at the dark
    // empty channel or the low-saturation white peak line — that bounds the
    // captured gradient.
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

    // Sample stop_count stops across the fill, low(bottom)→high(top).
    for (int i = 0; i < stop_count; ++i) {
        float frac = static_cast<float>(i) / static_cast<float>(stop_count - 1);
        int y = static_cast<int>(std::lround(fill_bottom - frac * fill_h));
        y = std::clamp(y, top, bottom - 1);
        out.gradient.push_back(to_color(pixel(img, cx, y)));
    }
    return out;
}

}  // namespace pulp::view
