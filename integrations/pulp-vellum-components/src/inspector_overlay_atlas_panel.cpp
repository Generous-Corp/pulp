#include <pulp/vellum_components/inspector_overlay.hpp>

#include <pulp/vellum_components/atlas_inventory.hpp>
#include <pulp/vellum_components/overlay_palette.hpp>

#include <algorithm>
#include <sstream>
#include <string>

namespace pulp::vellum_components {

void InspectorOverlay::paint_atlas_panel(canvas::Canvas& canvas) {
    atlas_row_count_ = 0;
    static const std::vector<overlay::AtlasInfo> kNoAtlases;
    const auto& atlases = atlas_inventory_ ? atlas_inventory_->atlases() : kNoAtlases;
    const float w = kPanelWidth;
    const float x = root_.bounds().width - w - kPanelMargin;
    const float y = kPanelMargin;
    const float h = std::min(overlay::atlas_panel_height(atlases.size()),
                             std::max(0.0f, root_.bounds().height - 2 * kPanelMargin));
    canvas.save();
    canvas.set_fill_color(overlay::kPanelBackground);
    canvas.fill_rounded_rect(x, y, w, h, 4.0f);
    canvas.clip_rect(x, y, w, h);

    const float left = x + kPanelPadding;
    const float inner_w = w - 2 * kPanelPadding;
    constexpr float kLine = 15.0f;
    canvas.set_font("monospace", kFontSize);
    canvas.set_fill_color(overlay::kPanelHeading);
    canvas.fill_text("Texture Atlases (A)", left, y + 4 + 11);

    // No inventory at all means nothing supplies one; an empty one means a
    // host supplies it and nothing is registered. They read differently so a
    // missing wire is not mistaken for a GPU fault.
    if (!atlas_inventory_) {
        canvas.set_fill_color(overlay::kPanelDim);
        canvas.fill_text("no atlas inventory attached", left, y + 4 + 11 + kLine);
        canvas.restore();
        return;
    }
    if (atlases.empty()) {
        canvas.set_fill_color(overlay::kPanelDim);
        canvas.fill_text("GPU atlas unavailable", left, y + 4 + 11 + kLine);
        canvas.restore();
        return;
    }
    atlas_row_count_ = atlases.size();

    // The totals, right-aligned on the heading line.
    {
        std::ostringstream summary;
        summary << atlas_inventory_->total_pages() << " pages  "
                << atlas_inventory_->total_entries() << " entries";
        canvas.set_fill_color(overlay::kPanelDim);
        const float sw = canvas.measure_text(summary.str());
        canvas.fill_text(summary.str(), left + inner_w - sw, y + 4 + 11);
    }

    float row_y = y + 4 + kLine;
    constexpr float kRowHeight = kLine * 2.0f + 6.0f;
    for (const auto& a : atlases) {
        if (row_y > y + h)
            break;
        std::ostringstream dims;
        dims << a.label << "  " << a.width << "x" << a.height;
        if (a.pages > 1)
            dims << " x" << a.pages << "p";
        canvas.set_fill_color(overlay::kPanelText);
        canvas.fill_text(dims.str(), left, row_y + 12);

        std::ostringstream entries;
        entries << a.entries << " entries";
        canvas.set_fill_color(overlay::kPanelDim);
        canvas.fill_text(entries.str(), left + inner_w - canvas.measure_text(entries.str()),
                         row_y + 12);

        // The occupancy bar turns amber, then red, as the atlas fills and is
        // closer to thrashing.
        const int pct = a.occupancy_percent();
        const canvas::Color& fill = pct >= 85   ? overlay::kBarHigh
                                    : pct >= 60 ? overlay::kBarMid
                                                : overlay::kBarLow;
        const float bar_y = row_y + kLine;
        constexpr float kBarHeight = 8.0f;
        const std::string percent = std::to_string(pct) + "%";
        const float percent_w = canvas.measure_text(percent) + 6.0f;
        const float bar_w = inner_w - percent_w;
        canvas.set_fill_color(overlay::kBarTrack);
        canvas.fill_rounded_rect(left, bar_y, bar_w, kBarHeight, 2.0f);
        if (pct > 0) {
            canvas.set_fill_color(fill);
            canvas.fill_rounded_rect(left, bar_y,
                                     std::max(2.0f, bar_w * static_cast<float>(pct) / 100.0f),
                                     kBarHeight, 2.0f);
        }
        canvas.set_fill_color(fill);
        canvas.fill_text(percent, left + inner_w - percent_w + 6.0f, bar_y + kBarHeight);
        row_y += kRowHeight;
    }
    canvas.restore();
}

} // namespace pulp::vellum_components
