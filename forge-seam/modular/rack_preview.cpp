#include "forge/rack_preview.hpp"

#include <forge/design_tokens.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace forge_modular {

namespace color = forge::design::color;

using pulp::canvas::Canvas;
using pulp::canvas::Color;

namespace {

Color from_rgb(std::uint32_t rgb, float alpha) {
    return Color::rgba8(static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
                        static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
                        static_cast<std::uint8_t>(rgb & 0xFF),
                        static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
}

/// A quadratic curve, flattened into a polyline. stroke_path keeps the joins,
/// which individually stroked segments would not -- a cable with visible kinks
/// reads as a diagram, not a cable.
void stroke_curve(Canvas& canvas, const CableCurve& c, Color stroke, float width,
                  float dy) {
    constexpr int kSegments = 24;
    Canvas::Point2D pts[kSegments + 1];
    for (int i = 0; i <= kSegments; ++i) {
        const float t = static_cast<float>(i) / kSegments;
        const float u = 1.0f - t;
        pts[i].x = u * u * c.x1 + 2 * u * t * c.cx + t * t * c.x2;
        pts[i].y = u * u * (c.y1 + dy) + 2 * u * t * (c.cy + dy) +
                   t * t * (c.y2 + dy);
    }
    canvas.set_stroke_color(stroke);
    canvas.set_line_width(width);
    canvas.set_line_cap(pulp::canvas::LineCap::round);
    canvas.stroke_path(pts, kSegments + 1);
}

}  // namespace

const std::string& RackPreview::panel_svg(const std::string& slug) const {
    static const std::string kNone;
    if (panel_dir_.empty() || slug.empty()) return kNone;
    const auto it = panel_cache_.find(slug);
    if (it != panel_cache_.end()) return it->second;

    // Dark first: the app's stage is dark, and the light panel on it reads as
    // a mistake rather than a variant.
    std::string text;
    for (const char* suffix : {"-dark.svg", ".svg"}) {
        std::ifstream f(panel_dir_ + "/" + slug + suffix, std::ios::binary);
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
        break;
    }
    return panel_cache_.emplace(slug, std::move(text)).first->second;
}

const RackModule* RackPreview::find(const std::string& id) const {
    for (const auto& m : modules_)
        if (m.id == id) return &m;
    return nullptr;
}

void RackPreview::set_rack(std::vector<RackModule> modules,
                           std::vector<Connection> connections) {
    modules_ = std::move(modules);
    connections_ = std::move(connections);
    request_repaint();
}

void RackPreview::set_highlight(std::optional<std::size_t> index) {
    if (index && *index >= connections_.size()) index.reset();
    if (index == highlight_) return;
    highlight_ = index;
    request_repaint();
}

void RackPreview::highlight_role(std::optional<SignalRole> role) {
    if (role == highlight_role_) return;
    highlight_role_ = role;
    request_repaint();
}

void RackPreview::set_progress(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    if (t == progress_) return;
    progress_ = t;
    request_repaint();
}

RackLayout RackPreview::layout_for(float width, float height) const {
    return layout_rack(modules_, width, height);
}

float RackPreview::cable_alpha(std::size_t index) const {
    if (index >= connections_.size()) return 0.0f;
    // Nothing hovered is the normal state, and a rack nobody is pointing at
    // must not look faded.
    if (!highlight_ && !highlight_role_) return 1.0f;
    if (highlight_ && *highlight_ == index) return 1.0f;
    if (highlight_role_ && connections_[index].role == *highlight_role_) return 1.0f;
    return 0.35f;
}

void RackPreview::paint(Canvas& canvas) {
    const auto b = bounds();
    const auto L = layout_rack(modules_, b.width, b.height);

    for (const auto& panel : L.panels) {
        // A module whose artwork is missing gets a plain face rather than a
        // borrowed one: showing another module's panel would misidentify it.
        const auto* mod_for_panel = find(panel.id);
        // Our artwork, for our modules only. Model slugs are unique within a
        // plugin but not across the library -- Fundamental also ships VCO,
        // VCF, VCA and LFO -- so matching on the slug alone draws OUR panel on
        // a vendor's module. It looks plausible, has the wrong controls and
        // the wrong width, and is a confident lie about what is in the rack.
        static const std::string kOurs = "ForgeModular";
        const auto& svg = (mod_for_panel && mod_for_panel->brand == kOurs)
                              ? panel_svg(mod_for_panel->name)
                              : std::string{};
        if (!svg.empty()) {
            // Clipped to its own slot. Each panel's artwork paints a
            // background rect a millimetre PROUD of its viewBox on every side
            // -- deliberate, so a panel has no hairline seam when Rack butts it
            // against its neighbour -- and unclipped here that overhang draws
            // straight over the modules either side.
            canvas.save();
            canvas.clip_rect(panel.x, panel.y, panel.width, panel.height);
            const bool drawn =
                canvas.draw_svg(svg, panel.x, panel.y, panel.width, panel.height);
            canvas.restore();
            // The real face, knobs and all. Nothing else to draw for it.
            if (drawn) continue;
        }

        canvas.set_fill_color(panel.has_artwork ? color::surface_panel
                                                : color::surface_sunken);
        canvas.fill_rounded_rect(panel.x, panel.y, panel.width, panel.height,
                                 4.0f * L.scale);
        canvas.set_stroke_color(color::line);
        canvas.set_line_width(1.0f);
        canvas.stroke_rounded_rect(panel.x, panel.y, panel.width, panel.height,
                                   4.0f * L.scale);

        // Name the panel. A rack of anonymous rectangles cannot be checked
        // against what was asked for, which is most of what a preview is for.
        const auto* mod = find(panel.id);
        if (!mod) continue;
        canvas.set_font(forge::design::type::display, 12.0f * L.scale);
        canvas.set_fill_color(color::text);
        canvas.set_text_align(pulp::canvas::TextAlign::center);
        canvas.fill_text(mod->name, panel.x + panel.width / 2.0f,
                         panel.y + 22.0f * L.scale);
        canvas.set_font(forge::design::type::mono, 8.5f * L.scale);
        canvas.set_fill_color(color::text_faint);
        canvas.fill_text(mod->brand, panel.x + panel.width / 2.0f,
                         panel.y + panel.height - 12.0f * L.scale);
    }

    for (std::size_t i = 0; i < connections_.size(); ++i) {
        const auto& c = connections_[i];
        const auto from = port_point(L, modules_, c.from_module, c.from_port,
                                     c.to_module);
        const auto to = port_point(L, modules_, c.to_module, c.to_port,
                                   c.from_module);
        const auto curve = cable_curve(from, to, progress_);
        const float a = cable_alpha(i);
        const auto rgb = role_color(c.role);

        // Three passes, as a real cable reads: a shadow it casts on the panel,
        // the cable, and a highlight along its top edge.
        stroke_curve(canvas, curve, Color::rgba8(0, 0, 0, static_cast<std::uint8_t>(0.42f * a * 255)),
                     5.4f * L.scale, 2.0f);
        stroke_curve(canvas, curve, from_rgb(rgb, a), 4.2f * L.scale, 0.0f);
        stroke_curve(canvas, curve,
                     Color::rgba8(255, 255, 255, static_cast<std::uint8_t>(0.20f * a * 255)),
                     1.2f * L.scale, -1.1f);

        // A jack ring at each end, so a docked end is visibly an edge dock
        // rather than a jack that happens to sit at the panel border.
        for (const auto& p : {from, to}) {
            if (p.docked) continue;
            canvas.set_stroke_color(from_rgb(rgb, a));
            canvas.set_line_width(1.5f);
            canvas.stroke_circle(p.x, p.y, 5.0f * L.scale);
        }
    }
}

}  // namespace forge_modular
