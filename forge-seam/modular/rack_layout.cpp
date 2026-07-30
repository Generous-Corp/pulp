#include "forge/rack_layout.hpp"

#include <algorithm>
#include <cmath>

namespace forge_modular {

std::uint32_t role_color(SignalRole role) {
    // The palette the patch itself carries: these are written into the colour
    // field at generation, so Rack shows exactly what the preview showed.
    switch (role) {
        case SignalRole::audio: return 0x00B56E;
        case SignalRole::pitch: return 0x3695EF;
        case SignalRole::clock: return 0xFFB437;
        case SignalRole::mod:   return 0x8B4ADE;
    }
    return 0x00B56E;
}

const PanelBox* RackLayout::panel(const std::string& id) const {
    for (const auto& p : panels)
        if (p.id == id) return &p;
    return nullptr;
}

RackLayout layout_rack(const std::vector<RackModule>& modules,
                       float viewport_width, float viewport_height) {
    RackLayout out;
    for (const auto& m : modules)
        out.total_width += static_cast<float>(m.hp) * kHorizontalPitch;
    if (modules.empty() || out.total_width <= 0.0f) return out;

    // Fit the whole strip, with breathing room, and never blow a small rack up
    // past a little over life size -- two modules stretched to fill a wide
    // window stop looking like Eurorack.
    const float fit_w = (viewport_width - 70.0f) / out.total_width;
    const float fit_h = (viewport_height - 110.0f) / kPanelHeight;
    out.scale = std::min({fit_w, fit_h, 1.05f});
    if (!(out.scale > 0.0f)) out.scale = 0.01f;   // a degenerate viewport, not a crash

    out.origin_x = (viewport_width - out.total_width * out.scale) / 2.0f;
    out.origin_y = (viewport_height - kPanelHeight * out.scale) / 2.0f - 10.0f;

    float x = out.origin_x;
    for (const auto& m : modules) {
        PanelBox box;
        box.id = m.id;
        box.x = x;
        box.y = out.origin_y;
        box.width = static_cast<float>(m.hp) * kHorizontalPitch * out.scale;
        box.height = kPanelHeight * out.scale;
        box.placed = m.placed;
        box.has_artwork = m.has_artwork;
        out.panels.push_back(box);
        x += box.width;   // butted, no gutter: that is what a rack does
    }
    return out;
}

namespace {

const RackModule* find_module(const std::vector<RackModule>& modules,
                              const std::string& id) {
    for (const auto& m : modules)
        if (m.id == id) return &m;
    return nullptr;
}

}  // namespace

JackPoint port_point(const RackLayout& layout,
                     const std::vector<RackModule>& modules,
                     const std::string& module_id, const std::string& port_id,
                     const std::string& other_module_id) {
    JackPoint pt;
    const auto* box = layout.panel(module_id);
    const auto* mod = find_module(modules, module_id);
    if (!box || !mod) return pt;

    std::size_t index = 0;
    const Port* port = nullptr;
    for (std::size_t i = 0; i < mod->ports.size(); ++i) {
        if (mod->ports[i].id == port_id) { port = &mod->ports[i]; index = i; break; }
    }

    // The ordinary case: a captured jack centre.
    if (mod->placed && port) {
        pt.x = box->x + port->x * box->width;
        pt.y = box->y + port->y * layout.scale;
        pt.name = port->name;
        return pt;
    }

    // Never placed, so there are no coordinates to use. Dock at the edge facing
    // the partner rather than guess a position -- truthful, and it resolves the
    // first time the module is placed. Ends are fanned by port order so several
    // cables do not all land on one spot.
    const auto* other = layout.panel(other_module_id);
    const float mid = box->x + box->width / 2.0f;
    const bool partner_is_left =
        other ? (other->x + other->width / 2.0f) < mid : true;

    const float step = std::max(11.0f, 19.0f * layout.scale);
    const float n = static_cast<float>(std::max<std::size_t>(mod->ports.size(), 1));
    pt.x = partner_is_left ? box->x + 3.0f : box->x + box->width - 3.0f;
    pt.y = box->y + box->height * 0.56f +
           (static_cast<float>(index) - (n - 1.0f) / 2.0f) * step;
    pt.name = port ? port->name : port_id;
    pt.docked = true;
    return pt;
}

CableCurve cable_curve(const JackPoint& from, const JackPoint& to, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    CableCurve c;
    c.x1 = from.x;
    c.y1 = from.y;
    // Partway through a build the cable has only reached partway across.
    c.x2 = from.x + (to.x - from.x) * t;
    c.y2 = from.y + (to.y - from.y) * t;

    const float dx = c.x2 - c.x1, dy = c.y2 - c.y1;
    const float distance = std::sqrt(dx * dx + dy * dy);
    // A longer cable hangs lower, up to a limit -- past that it is a coil on the
    // floor, not a curve. Scaled by t so a cable being drawn does not start at
    // full droop and straighten.
    const float sag = std::min(160.0f, 46.0f + distance * 0.36f) * t;
    c.cx = (c.x1 + c.x2) / 2.0f;
    c.cy = std::max(c.y1, c.y2) + sag * 0.86f;
    return c;
}

}  // namespace forge_modular
