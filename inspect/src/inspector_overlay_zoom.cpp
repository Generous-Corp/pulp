// inspector_overlay_zoom.cpp — Phase 3e 20× zoom loupe for the visual
// inspector overlay.
//
// Extracted from inspector_overlay.cpp in the 2026-05 refactor (roadmap
// P10-2). Pure mechanical move — the InspectorOverlay member methods
// below are byte-identical to their previous definitions in
// inspector_overlay.cpp; only their translation unit changed. Shared
// color constants live in inspector_overlay_internal.hpp; the
// structural zoom constants (kZoomGridCells, kZoomFactorMin/Max,
// kZoomReadoutH, kZoomPanelMargin) are static-constexpr members of
// InspectorOverlay reached through the public header.

#include "inspector_overlay_internal.hpp"

#include <pulp/inspect/inspector_overlay.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <sstream>
#include <vector>

namespace pulp::inspect {

// ── Phase 3e — 20× zoom loupe ───────────────────────────────────────────────
//
// A digital loupe: a fixed-corner panel showing the pixels under the
// cursor blown up by `zoom_factor_`, with a center crosshair on the
// exact sample pixel and a coordinate + hex readout. It pairs with the
// eyedropper — the eyedropper grabs one pixel, the loupe shows the
// neighborhood so edge alignment and color boundaries are visible.

void InspectorOverlay::set_zoom_active(bool active) {
    zoom_active_ = active;
    if (active) {
        // Seed the sample center so the first paint has a sane region
        // even before the cursor moves — the panel center is a safe,
        // always-on-screen default.
        zoom_sample_center_ = {root_.bounds().width * 0.5f,
                               root_.bounds().height * 0.5f};
    }
}

void InspectorOverlay::set_zoom_factor(int factor) {
    zoom_factor_ = std::clamp(factor, kZoomFactorMin, kZoomFactorMax);
}

// Walk the view tree top-most-first; return the deepest View that both
// contains `p` AND carries an explicit background color. This is the
// no-readback fallback: when read_pixels() isn't available we paint the
// loupe with the resolved authored colors instead of true device pixels.
bool InspectorOverlay::resolve_view_color_at(Point p, Color& out) const {
    const View* best = nullptr;
    std::function<void(const View*)> walk = [&](const View* v) {
        if (!v) return;
        auto r = view_bounds_in_root(v);
        bool inside = p.x >= r.x && p.x < r.x + r.width &&
                      p.y >= r.y && p.y < r.y + r.height;
        if (inside && v->has_background_color()) best = v;
        // Children paint over parents — visiting them after the parent
        // means a deeper hit overwrites `best`, matching paint order.
        for (size_t i = 0; i < v->child_count(); ++i) walk(v->child_at(i));
    };
    walk(&root_);
    if (!best) return false;
    out = best->background_color();
    return true;
}

// Refresh zoom_center_color_ for the current sample center. Tries a
// real pixel readback first (Skia raster only); falls back to resolved
// view color, and records which path ran in zoom_center_from_readback_
// so the readout can label degraded samples honestly.
void InspectorOverlay::update_zoom_sample(Canvas& canvas) {
    // The overlay paints onto a surface sized to the root view, so the
    // root bounds are the readback clamp limits. Clamp the sample pixel
    // into [0, w-1] × [0, h-1] — a raw read at an off-canvas coord (the
    // cursor sitting on the very edge) fails outright, which would push
    // the readout onto the degraded fallback path even on a readback-
    // capable surface. Clamping keeps the center readout honest about
    // readback at edges, and keeps it consistent with the block read in
    // paint_zoom_panel() (which clamps to the same bounds).
    const int cw = static_cast<int>(root_.bounds().width);
    const int ch = static_cast<int>(root_.bounds().height);
    int cx = static_cast<int>(zoom_sample_center_.x);
    int cy = static_cast<int>(zoom_sample_center_.y);
    if (cw > 0) cx = std::clamp(cx, 0, cw - 1);
    if (ch > 0) cy = std::clamp(cy, 0, ch - 1);

    std::uint8_t rgba[4] = {0, 0, 0, 0};
    if (canvas.read_pixels(cx, cy, 1, 1, rgba)) {
        zoom_center_color_ = Color::rgba(rgba[0] / 255.0f, rgba[1] / 255.0f,
                                         rgba[2] / 255.0f, rgba[3] / 255.0f);
        zoom_center_from_readback_ = true;
        return;
    }
    // No pixel readback on this surface (RecordingCanvas, CG fallback,
    // headless). Degrade gracefully to the authored view color.
    Color resolved{};
    if (resolve_view_color_at(zoom_sample_center_, resolved)) {
        zoom_center_color_ = resolved;
    } else {
        zoom_center_color_ = Color::rgba(0, 0, 0, 0);  // nothing under cursor
    }
    zoom_center_from_readback_ = false;
}

void InspectorOverlay::paint_zoom_panel(Canvas& canvas) {
    // Resolve the center pixel + degradation state for this frame.
    update_zoom_sample(canvas);

    const int   cells = kZoomGridCells;          // odd → exact center cell
    const int   half  = cells / 2;
    const float cell  = static_cast<float>(zoom_factor_);
    const float grid_px = cell * cells;

    // ── Panel placement: fixed bottom-left corner. Fixed (vs cursor-
    // following) avoids flicker and never occludes the cursor target;
    // bottom-left stays clear of the props panel on the right.
    const float pad = 6.0f;
    const float panel_w = grid_px + pad * 2.0f;
    const float panel_h = grid_px + pad * 2.0f + kZoomReadoutH;
    const float panel_x = kZoomPanelMargin;
    const float panel_y = root_.bounds().height - panel_h - kZoomPanelMargin;
    const float grid_x  = panel_x + pad;
    const float grid_y  = panel_y + pad;

    canvas.save();

    // Panel background + border.
    canvas.set_fill_color(kZoomPanelBg);
    canvas.fill_rect(panel_x, panel_y, panel_w, panel_h);
    canvas.set_stroke_color(kZoomBorder);
    canvas.set_line_width(1.5f);
    canvas.stroke_rect(panel_x, panel_y, panel_w, panel_h);

    // ── Magnified pixel grid ────────────────────────────────────────
    // Each cell maps to one source pixel. With a real readback we ask
    // the canvas for the whole NxN block at once; otherwise we
    // synthesize a checkerboard (with the resolved center color in the
    // middle) so the loupe still communicates "this is where you're
    // sampling".
    //
    // Edge clamping (codex P2 #2464): a window centered on a cursor
    // within `half` pixels of any canvas edge would put `ox`/`oy`
    // negative or push `ox+cells`/`oy+cells` past the surface — Skia's
    // readPixels() rejects an out-of-bounds source rect outright, so
    // the WHOLE block dropped to checkerboard exactly where pixel
    // inspection matters most. Clamp the read origin so the full
    // `cells × cells` window stays in-bounds; the magnified region
    // shifts slightly near edges but still shows real device pixels.
    // The sample pixel may then sit off-center within the grid, so the
    // crosshair below tracks its actual cell (cross_gx/cross_gy)
    // instead of assuming the middle.
    const int   cw = static_cast<int>(root_.bounds().width);
    const int   ch = static_cast<int>(root_.bounds().height);
    int ox = static_cast<int>(zoom_sample_center_.x) - half;
    int oy = static_cast<int>(zoom_sample_center_.y) - half;
    if (cw >= cells) ox = std::clamp(ox, 0, cw - cells);
    if (ch >= cells) oy = std::clamp(oy, 0, ch - cells);

    // The sample pixel's cell within the (possibly clamped) grid. When
    // the window isn't clamped this is the exact center (half, half);
    // near an edge it shifts toward the edge the cursor approached.
    int sample_px = std::clamp(static_cast<int>(zoom_sample_center_.x),
                               0, cw > 0 ? cw - 1 : 0);
    int sample_py = std::clamp(static_cast<int>(zoom_sample_center_.y),
                               0, ch > 0 ? ch - 1 : 0);
    const int cross_gx = std::clamp(sample_px - ox, 0, cells - 1);
    const int cross_gy = std::clamp(sample_py - oy, 0, cells - 1);

    std::vector<std::uint8_t> block(static_cast<size_t>(cells) * cells * 4, 0);
    const bool have_block = canvas.read_pixels(ox, oy, cells, cells,
                                               block.data());

    for (int gy = 0; gy < cells; ++gy) {
        for (int gx = 0; gx < cells; ++gx) {
            Color c;
            if (have_block) {
                const size_t i = (static_cast<size_t>(gy) * cells + gx) * 4;
                c = Color::rgba(block[i] / 255.0f, block[i + 1] / 255.0f,
                                block[i + 2] / 255.0f, block[i + 3] / 255.0f);
            } else if (gx == cross_gx && gy == cross_gy) {
                // Center (sample) cell shows the resolved sample color
                // even on the fallback path so the readout and grid
                // agree — and so they agree about WHICH cell is the
                // sample pixel when the window is edge-clamped.
                c = zoom_center_color_;
            } else {
                c = ((gx + gy) & 1) ? kZoomCheckerB : kZoomCheckerA;
            }
            canvas.set_fill_color(c);
            canvas.fill_rect(grid_x + gx * cell, grid_y + gy * cell,
                             cell, cell);
        }
    }

    // ── Pixel grid lines ────────────────────────────────────────────
    // Thin lines between magnified pixels so individual pixels read as
    // discrete cells, like Photoshop's pixel-grid at high zoom.
    canvas.set_stroke_color(kZoomGridLine);
    canvas.set_line_width(1.0f);
    for (int i = 0; i <= cells; ++i) {
        const float gx = grid_x + i * cell;
        const float gyl = grid_y + i * cell;
        canvas.stroke_line(gx, grid_y, gx, grid_y + grid_px);
        canvas.stroke_line(grid_x, gyl, grid_x + grid_px, gyl);
    }

    // ── Center crosshair / target ───────────────────────────────────
    // Outlines the exact sample pixel — the cell the readout describes.
    // Uses cross_gx/cross_gy (not `half`) so the crosshair still marks
    // the true sample pixel when the read window was edge-clamped.
    const float center_x = grid_x + cross_gx * cell;
    const float center_y = grid_y + cross_gy * cell;
    canvas.set_stroke_color(kZoomCrosshair);
    canvas.set_line_width(2.0f);
    canvas.stroke_rect(center_x, center_y, cell, cell);
    // Crosshair arms reaching from the grid edges toward the center
    // cell, so the eye is guided to the sample pixel.
    const float cxm = center_x + cell * 0.5f;
    const float cym = center_y + cell * 0.5f;
    canvas.set_line_width(1.0f);
    canvas.stroke_line(grid_x, cym, center_x, cym);
    canvas.stroke_line(center_x + cell, cym, grid_x + grid_px, cym);
    canvas.stroke_line(cxm, grid_y, cxm, center_y);
    canvas.stroke_line(cxm, center_y + cell, cxm, grid_y + grid_px);

    // ── Coordinate + hex readout strip ──────────────────────────────
    const float readout_y = grid_y + grid_px + 2.0f;
    canvas.set_font("monospace", 10.0f);

    const int sx = static_cast<int>(zoom_sample_center_.x);
    const int sy = static_cast<int>(zoom_sample_center_.y);
    std::ostringstream coord;
    coord << zoom_factor_ << "x  (" << sx << ", " << sy << ")";
    canvas.set_fill_color(kZoomReadoutText);
    canvas.fill_text(coord.str(), grid_x, readout_y + 12.0f);

    // Hex color readout — #RRGGBB plus an alpha byte when not opaque.
    auto to_byte = [](float v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    const int hr = to_byte(zoom_center_color_.r);
    const int hg = to_byte(zoom_center_color_.g);
    const int hb = to_byte(zoom_center_color_.b);
    const int ha = to_byte(zoom_center_color_.a);
    std::ostringstream hex;
    hex << '#' << std::hex << std::uppercase << std::setfill('0')
        << std::setw(2) << hr << std::setw(2) << hg << std::setw(2) << hb;
    if (ha != 255) hex << std::setw(2) << ha;

    // Color swatch + hex text on the second readout line.
    const float swatch = 10.0f;
    const float swatch_y = readout_y + 16.0f;
    canvas.set_fill_color(zoom_center_color_);
    canvas.fill_rect(grid_x, swatch_y, swatch, swatch);
    canvas.set_stroke_color(kZoomReadoutDim);
    canvas.set_line_width(1.0f);
    canvas.stroke_rect(grid_x, swatch_y, swatch, swatch);
    canvas.set_fill_color(kZoomReadoutText);
    canvas.fill_text(hex.str(), grid_x + swatch + 6.0f, swatch_y + 9.0f);

    // Honesty label: tell the user when the magnified grid is a
    // fallback render rather than true device pixels, so they don't
    // trust a checker pattern as real output. Keyed off `have_block`
    // (the grid's actual readback result) rather than just the center
    // pixel's `zoom_center_from_readback_` — with edge clamping the
    // two now agree on any normal surface, but on a degenerate canvas
    // narrower than the grid the 1×1 center read can still succeed
    // while the cells×cells block cannot, and the label must describe
    // the grid the user is looking at.
    if (!have_block) {
        canvas.set_fill_color(kZoomReadoutDim);
        canvas.fill_text("no readback", grid_x + grid_px - 64.0f,
                         swatch_y + 9.0f);
    }

    canvas.restore();
}

} // namespace pulp::inspect
