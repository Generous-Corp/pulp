#pragma once

// The patch preview: a rack, drawn from real panels and real jack positions.
//
// It is a composite rather than a screenshot, and that is the whole point.
// Rack can render each installed module to a PNG but has no facility to
// capture an assembled patch, so the panels are Rack's own artwork laid out
// in the patch's order at their true widths, with the cables drawn by us
// between jack coordinates recorded from inside a running rack.
//
// The consequence worth having: a screenshot is pixels, and a composite knows
// which cable is which. Hovering a line in the explanation can light up its
// cable, and hovering a cable can light up its line. That is the core
// interaction of patch mode, and it is only possible because we draw the
// cables ourselves.
//
// Geometry arrives through the contract in `rack_geometry.py` rather than from
// the port map directly, so a change of source moves the adapter and not this.
//
// Two degradations are designed rather than accidental:
//
//   * A module Rack has never rendered has no image. It draws as a placeholder
//     at the correct width -- correct width matters, because the neighbours'
//     positions depend on it -- clearly marked as not-yet-drawn rather than
//     as broken.
//   * A module nobody has placed in a rack has no port coordinates. Its cables
//     dock at the panel edge instead of landing on a jack. A cable drawn
//     confidently into the wrong hole teaches somebody something false, which
//     is the one thing a teaching surface cannot do.

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace forge_modular {

/// One port, as the geometry contract describes it.
struct PreviewPort {
    int index = 0;
    bool is_input = false;
    std::string name;
    float x = 0.f;      ///< fraction of panel width
    float y = 0.f;      ///< px from the panel top
};

/// A module we know how to draw.
struct PreviewModule {
    std::string slug;           ///< "Fundamental/VCO"
    int hp = 0;
    float width = 0.f;          ///< px, authoritative
    float height = 380.f;
    std::string image;          ///< empty when Rack has never rendered it
    bool mapped = false;        ///< false when its ports were never recorded
    std::vector<PreviewPort> ports;
};

/// One module placed in a patch.
struct PlacedModule {
    long id = 0;
    std::string slug;
    float x = 0.f;              ///< px, left edge, assigned by layout
    float y = 0.f;
};

struct PreviewCable {
    long from_id = 0, to_id = 0;
    int from_port = 0, to_port = 0;
    uint32_t color = 0xFF3695EF;
    std::string role;           ///< which explanation group it belongs to
};

/// Ink & Signal, so the preview belongs to the same product as the chat.
struct PreviewTheme {
    uint32_t background = 0xFF161A21;
    uint32_t rail = 0xFF28303C;
    uint32_t placeholder = 0xFF1E2530;
    uint32_t placeholder_line = 0x40D6DCE4;
    uint32_t text = 0xFF939CA9;
    uint32_t highlight = 0xFF16DAC2;
};

class PatchPreview : public pulp::view::View {
public:
    /// Geometry, keyed by "plugin/model". Comes from the contract adapter.
    void set_catalog(std::map<std::string, PreviewModule> catalog) {
        catalog_ = std::move(catalog);
    }

    void set_patch(std::vector<PlacedModule> modules,
                   std::vector<PreviewCable> cables) {
        modules_ = std::move(modules);
        cables_ = std::move(cables);
        layout();
    }

    /// Light one cable, and dim the rest. Driven by hovering its line in the
    /// explanation; the reverse direction sets the same field.
    void highlight(int cable_index) { highlighted_ = cable_index; }

    /// Total size of the rack at 1:1, before any fitting.
    float content_width() const { return content_w_; }
    float content_height() const { return content_h_; }

    void paint(pulp::canvas::Canvas& c) override {
        const auto b = bounds();
        c.set_fill_color(pulp::canvas::Color::from_argb32(theme_.background));
        c.fill_rect(0, 0, b.width, b.height);

        // A rack is a 3:1 strip and a pane rarely is, so fit rather than crop:
        // seeing the whole patch matters more than seeing it at full size,
        // since the explanation beside it describes all of it.
        const float scale = content_w_ > 0.f
            ? std::min(1.0f, std::min(b.width / content_w_, b.height / content_h_))
            : 1.0f;
        const float ox = (b.width - content_w_ * scale) * 0.5f;
        const float oy = (b.height - content_h_ * scale) * 0.5f;

        for (const auto& m : modules_) paint_module(c, m, scale, ox, oy);
        for (size_t i = 0; i < cables_.size(); ++i)
            paint_cable(c, cables_[i], static_cast<int>(i), scale, ox, oy);
    }

private:
    void layout() {
        // Left to right in the patch's own order, which is how Rack lays a
        // rack out and therefore how the user will see it.
        std::sort(modules_.begin(), modules_.end(),
                  [](const PlacedModule& a, const PlacedModule& b) {
                      return a.x < b.x;
                  });
        float pen = 0.f;
        content_h_ = 0.f;
        for (auto& m : modules_) {
            const auto* g = geometry(m.slug);
            const float w = g ? g->width : 120.f;
            const float h = g ? g->height : 380.f;
            m.x = pen;
            m.y = 0.f;
            pen += w;
            content_h_ = std::max(content_h_, h);
        }
        content_w_ = pen;
    }

    const PreviewModule* geometry(const std::string& slug) const {
        auto it = catalog_.find(slug);
        return it == catalog_.end() ? nullptr : &it->second;
    }

    const PlacedModule* placed(long id) const {
        for (const auto& m : modules_)
            if (m.id == id) return &m;
        return nullptr;
    }

    void paint_module(pulp::canvas::Canvas& c, const PlacedModule& m,
                      float s, float ox, float oy) const {
        const auto* g = geometry(m.slug);
        const float w = (g ? g->width : 120.f) * s;
        const float h = (g ? g->height : 380.f) * s;
        const float x = ox + m.x * s;
        const float y = oy + m.y * s;

        if (g && !g->image.empty()) {
            if (c.draw_image_from_file(g->image, x, y, w, h)) return;
            // Falling through means the file went away between the catalog
            // being built and now; a placeholder is better than a gap.
        }
        paint_placeholder(c, m.slug, x, y, w, h);
    }

    void paint_placeholder(pulp::canvas::Canvas& c, const std::string& slug,
                           float x, float y, float w, float h) const {
        c.set_fill_color(pulp::canvas::Color::from_argb32(theme_.placeholder));
        c.fill_rect(x, y, w, h);
        c.set_stroke_color(pulp::canvas::Color::from_argb32(theme_.placeholder_line));
        c.set_line_width(1.f);
        c.stroke_rect(x + 0.5f, y + 0.5f, w - 1.f, h - 1.f);
        // The model name only; the plugin prefix is what makes these
        // unreadable at the widths a rack forces.
        const auto slash = slug.rfind('/');
        const std::string label =
            slash == std::string::npos ? slug : slug.substr(slash + 1);
        c.set_fill_color(pulp::canvas::Color::from_argb32(theme_.text));
        // Centred by hand: the canvas draws from a baseline.
        const float tw = c.measure_text(label);
        c.fill_text(label, x + (w - tw) * 0.5f, y + h * 0.5f);
    }

    /// Where a cable meets a module, in view coordinates.
    ///
    /// Falls back to the panel's bottom edge for a module whose ports were
    /// never recorded, spreading endpoints across the width so several cables
    /// to the same unmapped module stay distinguishable.
    void endpoint(const PlacedModule& m, int port, bool is_input,
                  float s, float ox, float oy, float& out_x, float& out_y) const {
        const auto* g = geometry(m.slug);
        const float w = g ? g->width : 120.f;
        const float h = g ? g->height : 380.f;
        if (g && g->mapped) {
            for (const auto& p : g->ports) {
                if (p.index == port && p.is_input == is_input) {
                    out_x = ox + (m.x + p.x * w) * s;
                    out_y = oy + p.y * s;
                    return;
                }
            }
        }
        const float slot = 0.2f + 0.15f * static_cast<float>(port % 5);
        out_x = ox + (m.x + w * slot) * s;
        out_y = oy + (h - 6.f) * s;
    }

    void paint_cable(pulp::canvas::Canvas& c, const PreviewCable& cable,
                     int index, float s, float ox, float oy) const {
        const auto* from = placed(cable.from_id);
        const auto* to = placed(cable.to_id);
        if (!from || !to) return;   // the lint reports these; do not draw them

        float x0, y0, x1, y1;
        endpoint(*from, cable.from_port, false, s, ox, oy, x0, y0);
        endpoint(*to, cable.to_port, true, s, ox, oy, x1, y1);

        const bool dim = highlighted_ >= 0 && highlighted_ != index;
        uint32_t col = cable.color;
        if (dim) col = (col & 0x00FFFFFF) | 0x40000000;
        else if (highlighted_ == index) col = theme_.highlight;

        // Cables hang. The sag scales with span so a short patch cable does
        // not loop absurdly and a long one does not read as a taut wire.
        const float span = std::fabs(x1 - x0);
        const float sag = std::min(90.f, 26.f + span * 0.22f) * s;

        c.begin_path();
        c.move_to(x0, y0);
        c.cubic_to(x0, y0 + sag, x1, y1 + sag, x1, y1);
        c.set_stroke_color(pulp::canvas::Color::from_argb32(col));
        c.set_line_width((highlighted_ == index ? 3.4f : 2.4f) * s);
        c.stroke_current_path();
    }

    std::map<std::string, PreviewModule> catalog_;
    std::vector<PlacedModule> modules_;
    std::vector<PreviewCable> cables_;
    PreviewTheme theme_;
    float content_w_ = 0.f, content_h_ = 0.f;
    int highlighted_ = -1;
};

}  // namespace forge_modular
