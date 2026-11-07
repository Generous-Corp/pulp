// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include <pulp/audio/analysis/raster_image.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace pulp::test::audio {

namespace {

// Printable ASCII 0x20..0x7E as 5x7 row bitmaps, bit 4 = leftmost column.
// Index = c - 0x20. The final entry is the replacement box used for any
// byte outside the table.
constexpr std::array<std::array<std::uint8_t, 7>, 96> kFont5x7 = {{
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // ' '
    {0x04, 0x04, 0x04, 0x04, 0x00, 0x00, 0x04},  // '!'
    {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00},  // '"'
    {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A},  // '#'
    {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04},  // '$'
    {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03},  // '%'
    {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D},  // '&'
    {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00},  // '\''
    {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},  // '('
    {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},  // ')'
    {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00},  // '*'
    {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00},  // '+'
    {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08},  // ','
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},  // '-'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},  // '.'
    {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00},  // '/'
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // '0'
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // '1'
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // '2'
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // '3'
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // '4'
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // '5'
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // '6'
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // '7'
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // '8'
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // '9'
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00},  // ':'
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x04, 0x08},  // ';'
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},  // '<'
    {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},  // '='
    {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},  // '>'
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},  // '?'
    {0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E},  // '@'
    {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11},  // 'A'
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},  // 'B'
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},  // 'C'
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C},  // 'D'
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},  // 'E'
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},  // 'F'
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},  // 'G'
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // 'H'
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 'I'
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},  // 'J'
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},  // 'K'
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},  // 'L'
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},  // 'M'
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},  // 'N'
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // 'O'
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},  // 'P'
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},  // 'Q'
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},  // 'R'
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},  // 'S'
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // 'T'
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // 'U'
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},  // 'V'
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},  // 'W'
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},  // 'X'
    {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04},  // 'Y'
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},  // 'Z'
    {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E},  // '['
    {0x00, 0x10, 0x08, 0x04, 0x02, 0x01, 0x00},  // '\\'
    {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E},  // ']'
    {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00},  // '^'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F},  // '_'
    {0x08, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00},  // '`'
    {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F},  // 'a'
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x1E},  // 'b'
    {0x00, 0x00, 0x0E, 0x10, 0x10, 0x11, 0x0E},  // 'c'
    {0x01, 0x01, 0x0D, 0x13, 0x11, 0x11, 0x0F},  // 'd'
    {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E},  // 'e'
    {0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08},  // 'f'
    {0x00, 0x00, 0x0F, 0x11, 0x0F, 0x01, 0x0E},  // 'g'
    {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11},  // 'h'
    {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E},  // 'i'
    {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C},  // 'j'
    {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12},  // 'k'
    {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 'l'
    {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11},  // 'm'
    {0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11},  // 'n'
    {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E},  // 'o'
    {0x00, 0x00, 0x1E, 0x11, 0x11, 0x1E, 0x10},  // 'p'
    {0x00, 0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01},  // 'q'
    {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10},  // 'r'
    {0x00, 0x00, 0x0E, 0x10, 0x0E, 0x01, 0x1E},  // 's'
    {0x08, 0x08, 0x1C, 0x08, 0x08, 0x09, 0x06},  // 't'
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D},  // 'u'
    {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04},  // 'v'
    {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A},  // 'w'
    {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11},  // 'x'
    {0x00, 0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E},  // 'y'
    {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F},  // 'z'
    {0x02, 0x04, 0x04, 0x08, 0x04, 0x04, 0x02},  // '{'
    {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // '|'
    {0x08, 0x04, 0x04, 0x02, 0x04, 0x04, 0x08},  // '}'
    {0x00, 0x00, 0x08, 0x15, 0x02, 0x00, 0x00},  // '~'
    {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F},  // replacement box
}};

}  // namespace

RasterImage RasterImage::filled(std::uint32_t width, std::uint32_t height, Rgb8 colour) {
    RasterImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        image.rgba[i + 0] = colour.r;
        image.rgba[i + 1] = colour.g;
        image.rgba[i + 2] = colour.b;
        image.rgba[i + 3] = 255;
    }
    return image;
}

void RasterImage::put(int x, int y, Rgb8 colour) noexcept {
    if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
        return;
    const auto idx = (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
    rgba[idx + 0] = colour.r;
    rgba[idx + 1] = colour.g;
    rgba[idx + 2] = colour.b;
    rgba[idx + 3] = 255;
}

Rgb8 RasterImage::get(int x, int y) const noexcept {
    if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
        return {};
    const auto idx = (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
    return Rgb8{rgba[idx + 0], rgba[idx + 1], rgba[idx + 2]};
}

void RasterImage::fill_rect(int x0, int y0, int x1, int y1, Rgb8 colour) noexcept {
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, static_cast<int>(width));
    y1 = std::min(y1, static_cast<int>(height));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            put(x, y, colour);
}

const std::uint8_t* glyph_rows_5x7(char c) noexcept {
    const int code = static_cast<unsigned char>(c);
    if (code < 0x20 || code > 0x7E)
        return kFont5x7.back().data();
    return kFont5x7[static_cast<std::size_t>(code - 0x20)].data();
}

int text_width_5x7(std::string_view text, int scale) noexcept {
    if (text.empty())
        return 0;
    const int glyphs = static_cast<int>(text.size());
    return (glyphs * kGlyphAdvance - 1) * scale;
}

void draw_text_5x7(RasterImage& image, int x, int y, std::string_view text, Rgb8 colour,
                   int scale) noexcept {
    scale = std::max(scale, 1);
    int pen = x;
    for (char c : text) {
        const std::uint8_t* rows = glyph_rows_5x7(c);
        for (int row = 0; row < kGlyphHeight; ++row) {
            for (int col = 0; col < kGlyphWidth; ++col) {
                if ((rows[row] >> (kGlyphWidth - 1 - col)) & 1)
                    image.fill_rect(pen + col * scale, y + row * scale, pen + (col + 1) * scale,
                                    y + (row + 1) * scale, colour);
            }
        }
        pen += kGlyphAdvance * scale;
    }
}

double nice_tick_step(double lo, double hi, int max_ticks) noexcept {
    const double span = hi - lo;
    if (!(span > 0.0) || max_ticks < 1 || !std::isfinite(span))
        return 0.0;
    const double raw = span / static_cast<double>(max_ticks);
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    for (double multiplier : {1.0, 2.0, 5.0, 10.0}) {
        const double step = multiplier * magnitude;
        if (span / step <= static_cast<double>(max_ticks))
            return step;
    }
    return 10.0 * magnitude;
}

std::string format_seconds_label(double seconds, int decimals) {
    decimals = std::clamp(decimals, 0, 6);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, seconds);
    std::string text(buffer);
    if (decimals > 0) {
        while (!text.empty() && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.')
            text.pop_back();
    }
    if (text == "-0")
        text = "0";
    return text;
}

std::vector<AxisTick> time_axis_ticks(double start_s, double end_s, int max_ticks) {
    std::vector<AxisTick> ticks;
    const double step = nice_tick_step(start_s, end_s, max_ticks);
    if (step <= 0.0)
        return ticks;
    // Decimals needed to print the step exactly: 0.5 -> 1, 0.02 -> 2, 5 -> 0.
    int decimals = 0;
    for (double s = step; decimals < 6 && std::abs(s - std::round(s)) > 1e-9; s *= 10.0)
        ++decimals;
    const double first = std::ceil(start_s / step - 1e-9) * step;
    for (double t = first; t <= end_s + step * 1e-6; t += step) {
        const double snapped = std::round(t / step) * step;
        ticks.push_back(AxisTick{snapped, format_seconds_label(snapped, decimals)});
        if (ticks.size() > static_cast<std::size_t>(max_ticks) + 2)
            break;
    }
    return ticks;
}

std::uint32_t chrome_image_width(std::uint32_t plot_width) noexcept {
    return plot_width + static_cast<std::uint32_t>(kChromeLeftMargin + kChromeRightMargin);
}

std::uint32_t chrome_image_height(std::uint32_t plot_height) noexcept {
    return plot_height + static_cast<std::uint32_t>(kChromeTopMargin + kChromeTimeAxisHeight +
                                                    kChromeFooterHeight);
}

RasterImage compose_plot_with_chrome(const RasterImage& plot, const std::vector<AxisLabel>& left,
                                     const ImageChrome& chrome) {
    constexpr Rgb8 kBackground{14, 14, 18};
    constexpr Rgb8 kFrame{70, 76, 88};
    constexpr Rgb8 kText{200, 204, 212};
    constexpr Rgb8 kTick{120, 128, 140};

    const auto width = chrome_image_width(plot.width);
    const auto height = chrome_image_height(plot.height);
    RasterImage image = RasterImage::filled(width, height, kBackground);

    const int plot_x = kChromeLeftMargin;
    const int plot_y = kChromeTopMargin;
    const int plot_w = static_cast<int>(plot.width);
    const int plot_h = static_cast<int>(plot.height);

    for (int y = 0; y < plot_h; ++y)
        for (int x = 0; x < plot_w; ++x)
            image.put(plot_x + x, plot_y + y, plot.get(x, y));

    // Frame around the plot.
    image.fill_rect(plot_x - 1, plot_y - 1, plot_x + plot_w + 1, plot_y, kFrame);
    image.fill_rect(plot_x - 1, plot_y + plot_h, plot_x + plot_w + 1, plot_y + plot_h + 1, kFrame);
    image.fill_rect(plot_x - 1, plot_y - 1, plot_x, plot_y + plot_h + 1, kFrame);
    image.fill_rect(plot_x + plot_w, plot_y - 1, plot_x + plot_w + 1, plot_y + plot_h + 1, kFrame);

    // Left axis labels, right-aligned against the frame, vertically centred on
    // the tick row; labels that would collide with the previous one are
    // dropped so dense axes stay legible.
    int last_label_bottom = -1000;
    std::vector<AxisLabel> ordered = left;
    std::sort(ordered.begin(), ordered.end(),
              [](const AxisLabel& a, const AxisLabel& b) { return a.unit > b.unit; });
    for (const auto& label : ordered) {
        if (label.unit < 0.0 || label.unit > 1.0)
            continue;
        const int row = plot_y + static_cast<int>(std::round((1.0 - label.unit) * (plot_h - 1)));
        image.fill_rect(plot_x - 4, row, plot_x - 1, row + 1, kTick);
        const int text_top = row - kGlyphHeight / 2;
        if (text_top < last_label_bottom + 1)
            continue;
        const int text_w = text_width_5x7(label.label);
        draw_text_5x7(image, plot_x - 6 - text_w, text_top, label.label, kText);
        last_label_bottom = text_top + kGlyphHeight;
    }

    // Time axis along the bottom of the plot.
    const int axis_y = plot_y + plot_h + 2;
    const int max_ticks = std::max(2, plot_w / 90);
    int last_label_right = -1000;
    for (const auto& tick : time_axis_ticks(chrome.start_s, chrome.end_s, max_ticks)) {
        const double span = chrome.end_s - chrome.start_s;
        if (!(span > 0.0))
            break;
        const double unit = (tick.value - chrome.start_s) / span;
        if (unit < -1e-9 || unit > 1.0 + 1e-9)
            continue;
        const int col = plot_x + static_cast<int>(std::round(unit * (plot_w - 1)));
        image.fill_rect(col, axis_y - 1, col + 1, axis_y + 2, kTick);
        const int text_w = text_width_5x7(tick.label);
        int text_x = col - text_w / 2;
        text_x = std::clamp(text_x, plot_x, std::max(plot_x, plot_x + plot_w - text_w));
        if (text_x <= last_label_right + kGlyphAdvance)
            continue;
        draw_text_5x7(image, text_x, axis_y + 3, tick.label, kText);
        last_label_right = text_x + text_w;
    }

    // Footer: input identity and settings, burned in so a screenshot stays
    // self-describing. Clipped to the image width rather than wrapped.
    const int footer_y = static_cast<int>(height) - kChromeFooterHeight + 2;
    draw_text_5x7(image, 2, footer_y, chrome.footer, kText);
    return image;
}

std::string format_frequency_label(double hz) {
    if (hz >= 1000.0) {
        const double k = hz / 1000.0;
        const double rounded = std::round(k * 10.0) / 10.0;
        std::string text = format_seconds_label(rounded, 1);
        return text + "k";
    }
    return format_seconds_label(std::round(hz), 0);
}

}  // namespace pulp::test::audio
