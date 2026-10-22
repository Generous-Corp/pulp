#include "frame_stamp.hpp"

#include <cmath>

namespace pulp::test_support {

namespace {

// Luminance thresholds on 0..255. A cell between them is neither black nor
// white and makes the frame unreadable.
constexpr double kBlackMax = 64.0;
constexpr double kWhiteMin = 191.0;

// Both decoders return the index as a non-negative int beside -1, so the
// painter refuses the same widths they do.
constexpr int kMaxBits = 30;

uint32_t stamp_mask(int bits) {
    return (1u << bits) - 1u;
}

// Mean luminance over the central half of one cell; -1 when the sampled
// square falls outside the image.
double cell_luminance(const uint8_t* rgba, uint32_t width, uint32_t height, float scale,
                      const FrameStampLayout& layout, int col, int row) {
    const double side = static_cast<double>(layout.cell) * scale;
    const double x0 = (layout.x + col * layout.cell) * scale + side * 0.25;
    const double y0 = (layout.y + row * layout.cell) * scale + side * 0.25;
    const auto left = static_cast<long>(std::floor(x0));
    const auto top = static_cast<long>(std::floor(y0));
    const auto right = static_cast<long>(std::ceil(x0 + side * 0.5));
    const auto bottom = static_cast<long>(std::ceil(y0 + side * 0.5));
    if (left < 0 || top < 0 || right > static_cast<long>(width) ||
        bottom > static_cast<long>(height) || right <= left || bottom <= top)
        return -1.0;
    double sum = 0.0;
    long count = 0;
    for (long y = top; y < bottom; ++y) {
        for (long x = left; x < right; ++x) {
            const uint8_t* p = rgba + (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4;
            sum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
            ++count;
        }
    }
    return sum / static_cast<double>(count);
}

} // namespace

void paint_frame_stamp(canvas::Canvas& canvas, uint32_t index, const FrameStampLayout& layout) {
    if (layout.bits <= 0 || layout.bits > kMaxBits)
        return;
    const uint32_t value = index & stamp_mask(layout.bits);
    const auto white = canvas::Color::rgba8(255, 255, 255, 255);
    const auto black = canvas::Color::rgba8(0, 0, 0, 255);
    for (int col = 0; col < layout.bits; ++col) {
        const bool bit = ((value >> (layout.bits - 1 - col)) & 1u) != 0;
        for (int row = 0; row < 2; ++row) {
            const bool lit = row == 0 ? bit : !bit;
            canvas.set_fill_color(lit ? white : black);
            canvas.fill_rect(layout.x + col * layout.cell, layout.y + row * layout.cell,
                             layout.cell, layout.cell);
        }
    }
}

int decode_frame_stamp(const uint8_t* rgba, uint32_t width, uint32_t height, float scale,
                       const FrameStampLayout& layout) {
    if (!rgba || layout.bits <= 0 || layout.bits > kMaxBits || layout.cell <= 0.0f || scale <= 0.0f)
        return -1;
    uint32_t value = 0;
    for (int col = 0; col < layout.bits; ++col) {
        bool rows[2] = {false, false};
        for (int row = 0; row < 2; ++row) {
            const double lum = cell_luminance(rgba, width, height, scale, layout, col, row);
            if (lum < 0.0)
                return -1;
            if (lum >= kWhiteMin)
                rows[row] = true;
            else if (lum <= kBlackMax)
                rows[row] = false;
            else
                return -1;
        }
        if (rows[0] == rows[1])
            return -1;
        value = (value << 1) | (rows[0] ? 1u : 0u);
    }
    return static_cast<int>(value);
}

} // namespace pulp::test_support
