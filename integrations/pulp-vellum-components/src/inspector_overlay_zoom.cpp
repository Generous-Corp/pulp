// The overlay's pixel loupe: a panel in the bottom-left corner that magnifies
// the pixels around the pointer, marks the sampled pixel, and reads out its
// position and colour.

#include <pulp/vellum_components/inspector_overlay.hpp>

#include <pulp/vellum_components/overlay_palette.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

namespace pulp::vellum_components {

namespace {
using Color = canvas::Color;
}

void InspectorOverlay::set_zoom_active(bool active) {
    zoom_active_ = active;
    if (active) {
        // Start at the middle of the root so the first paint has a sensible
        // region before the pointer moves.
        zoom_sample_center_ = {root_.bounds().width * 0.5f, root_.bounds().height * 0.5f};
    }
}

void InspectorOverlay::set_zoom_factor(int factor) {
    zoom_factor_ = std::clamp(factor, kZoomFactorMin, kZoomFactorMax);
}

bool InspectorOverlay::resolve_view_color_at(Point p, Color& out) const {
    // The deepest view that contains `p` and has a background of its own. A
    // child paints over its parent, so visiting children after the parent lets
    // the deeper hit win, which is paint order.
    const View* best = nullptr;
    const auto walk = [&](const auto& self, const View* v) -> void {
        const auto r = overlay::bounds_in_root(v, root_);
        const bool inside = p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height;
        if (inside && v->has_background_color())
            best = v;
        for (std::size_t i = 0; i < v->child_count(); ++i)
            self(self, v->child_at(i));
    };
    walk(walk, &root_);
    if (!best)
        return false;
    out = best->background_color();
    return true;
}

void InspectorOverlay::update_zoom_sample(canvas::Canvas& canvas) {
    // The overlay paints onto a surface the size of the root, so the root is
    // the readback limit. A read off the surface fails outright, which would
    // push the readout onto the fallback on a surface that can read back, so
    // clamp the sampled pixel in-bounds.
    const int width = static_cast<int>(root_.bounds().width);
    const int height = static_cast<int>(root_.bounds().height);
    int x = static_cast<int>(zoom_sample_center_.x);
    int y = static_cast<int>(zoom_sample_center_.y);
    if (width > 0)
        x = std::clamp(x, 0, width - 1);
    if (height > 0)
        y = std::clamp(y, 0, height - 1);

    std::uint8_t rgba[4] = {0, 0, 0, 0};
    if (canvas.read_pixels(x, y, 1, 1, rgba)) {
        zoom_center_color_ =
            Color::rgba(rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f);
        return;
    }
    // No readback here (a recording canvas, a headless surface): fall back to
    // the colour the view tree declares under the pointer.
    Color resolved{};
    zoom_center_color_ = resolve_view_color_at(zoom_sample_center_, resolved)
                             ? resolved
                             : Color::rgba(0.0f, 0.0f, 0.0f, 0.0f);
}

void InspectorOverlay::paint_zoom_panel(canvas::Canvas& canvas) {
    update_zoom_sample(canvas);

    constexpr int cells = kZoomGridCells; // odd, so the middle cell is exact
    constexpr int half = cells / 2;
    const float cell = static_cast<float>(zoom_factor_);
    const float grid_px = cell * cells;

    // A fixed bottom-left corner never follows, so it never covers the pixel
    // being inspected.
    constexpr float pad = 6.0f;
    const float panel_w = grid_px + pad * 2.0f;
    const float panel_h = grid_px + pad * 2.0f + kZoomReadoutHeight;
    const float panel_x = kZoomPanelMargin;
    const float panel_y = root_.bounds().height - panel_h - kZoomPanelMargin;
    const float grid_x = panel_x + pad;
    const float grid_y = panel_y + pad;

    canvas.save();
    canvas.set_fill_color(overlay::kZoomPanelBackground);
    canvas.fill_rect(panel_x, panel_y, panel_w, panel_h);
    canvas.set_stroke_color(overlay::kZoomBorder);
    canvas.set_line_width(1.5f);
    canvas.stroke_rect(panel_x, panel_y, panel_w, panel_h);

    // Ask for the whole block at once. Within `half` pixels of an edge the
    // window would leave the surface and the read would be refused, so slide
    // it back in-bounds; the sampled pixel then sits off-centre in the grid
    // and the crosshair tracks its real cell.
    const int width = static_cast<int>(root_.bounds().width);
    const int height = static_cast<int>(root_.bounds().height);
    int origin_x = static_cast<int>(zoom_sample_center_.x) - half;
    int origin_y = static_cast<int>(zoom_sample_center_.y) - half;
    if (width >= cells)
        origin_x = std::clamp(origin_x, 0, width - cells);
    if (height >= cells)
        origin_y = std::clamp(origin_y, 0, height - cells);
    const int sample_x =
        std::clamp(static_cast<int>(zoom_sample_center_.x), 0, width > 0 ? width - 1 : 0);
    const int sample_y =
        std::clamp(static_cast<int>(zoom_sample_center_.y), 0, height > 0 ? height - 1 : 0);
    const int cross_x = std::clamp(sample_x - origin_x, 0, cells - 1);
    const int cross_y = std::clamp(sample_y - origin_y, 0, cells - 1);

    std::vector<std::uint8_t> block(static_cast<std::size_t>(cells) * cells * 4, 0);
    const bool have_block = canvas.read_pixels(origin_x, origin_y, cells, cells, block.data());

    for (int gy = 0; gy < cells; ++gy) {
        for (int gx = 0; gx < cells; ++gx) {
            Color c;
            if (have_block) {
                const std::size_t i = (static_cast<std::size_t>(gy) * cells + gx) * 4;
                c = Color::rgba(block[i] / 255.0f, block[i + 1] / 255.0f, block[i + 2] / 255.0f,
                                block[i + 3] / 255.0f);
            } else if (gx == cross_x && gy == cross_y) {
                // The sampled cell shows the resolved colour so the grid and
                // the readout agree on which cell is the sample.
                c = zoom_center_color_;
            } else {
                c = ((gx + gy) & 1) ? overlay::kZoomCheckerB : overlay::kZoomCheckerA;
            }
            canvas.set_fill_color(c);
            canvas.fill_rect(grid_x + gx * cell, grid_y + gy * cell, cell, cell);
        }
    }

    // Thin lines between the magnified pixels keep them discrete.
    canvas.set_stroke_color(overlay::kZoomGridLine);
    canvas.set_line_width(1.0f);
    for (int i = 0; i <= cells; ++i) {
        const float gx = grid_x + i * cell;
        const float gy = grid_y + i * cell;
        canvas.stroke_line(gx, grid_y, gx, grid_y + grid_px);
        canvas.stroke_line(grid_x, gy, grid_x + grid_px, gy);
    }

    // Outline the sampled pixel and run arms from the grid edges toward it.
    const float target_x = grid_x + cross_x * cell;
    const float target_y = grid_y + cross_y * cell;
    canvas.set_stroke_color(overlay::kZoomCrosshair);
    canvas.set_line_width(2.0f);
    canvas.stroke_rect(target_x, target_y, cell, cell);
    const float mid_x = target_x + cell * 0.5f;
    const float mid_y = target_y + cell * 0.5f;
    canvas.set_line_width(1.0f);
    canvas.stroke_line(grid_x, mid_y, target_x, mid_y);
    canvas.stroke_line(target_x + cell, mid_y, grid_x + grid_px, mid_y);
    canvas.stroke_line(mid_x, grid_y, mid_x, target_y);
    canvas.stroke_line(mid_x, target_y + cell, mid_x, grid_y + grid_px);

    // Readout: magnification and position, then a swatch with the hex colour.
    const float readout_y = grid_y + grid_px + 2.0f;
    canvas.set_font("monospace", 10.0f);
    std::ostringstream coordinates;
    coordinates << zoom_factor_ << "x  (" << static_cast<int>(zoom_sample_center_.x) << ", "
                << static_cast<int>(zoom_sample_center_.y) << ")";
    canvas.set_fill_color(overlay::kZoomReadoutText);
    canvas.fill_text(coordinates.str(), grid_x, readout_y + 12.0f);

    const auto to_byte = [](float v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    std::ostringstream hex;
    hex << '#' << std::hex << std::uppercase << std::setfill('0') << std::setw(2)
        << to_byte(zoom_center_color_.r) << std::setw(2) << to_byte(zoom_center_color_.g)
        << std::setw(2) << to_byte(zoom_center_color_.b);
    if (to_byte(zoom_center_color_.a) != 255)
        hex << std::setw(2) << to_byte(zoom_center_color_.a);

    constexpr float swatch = 10.0f;
    const float swatch_y = readout_y + 16.0f;
    canvas.set_fill_color(zoom_center_color_);
    canvas.fill_rect(grid_x, swatch_y, swatch, swatch);
    canvas.set_stroke_color(overlay::kZoomReadoutDim);
    canvas.set_line_width(1.0f);
    canvas.stroke_rect(grid_x, swatch_y, swatch, swatch);
    canvas.set_fill_color(overlay::kZoomReadoutText);
    canvas.fill_text(hex.str(), grid_x + swatch + 6.0f, swatch_y + 9.0f);

    // Say so when the grid is a stand-in, so a checkerboard is never mistaken
    // for real output. It follows the block read, which is what the grid shows.
    if (!have_block) {
        canvas.set_fill_color(overlay::kZoomReadoutDim);
        canvas.fill_text("no readback", grid_x + grid_px - 64.0f, swatch_y + 9.0f);
    }
    canvas.restore();
}

} // namespace pulp::vellum_components
