#include "forge/rack_layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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
    if (modules.empty()) return out;

    // Rack's own grid: the patch says where each module sits, in HP across and
    // rows down. Everything used to be laid end to end on one line regardless,
    // so a two-row rack drew as one very long strip and shrank to nothing --
    // and a patch built to be read in rows lost the arrangement that made it
    // readable.
    //
    // Modules without a position keep the old behaviour: appended to row 0 in
    // order. A patch that never said where anything goes has not got an
    // arrangement to lose.
    std::vector<std::pair<int, int>> cell(modules.size());   // (hp_x, row)
    int appended_x = 0;
    int min_x = 0, min_y = 0;
    bool first = true;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        const auto& m = modules[i];
        if (m.has_grid_pos) {
            cell[i] = {m.grid_x, m.grid_y};
        } else {
            cell[i] = {appended_x, 0};
            appended_x += std::max(1, m.hp);
        }
        if (first) { min_x = cell[i].first; min_y = cell[i].second; first = false; }
        min_x = std::min(min_x, cell[i].first);
        min_y = std::min(min_y, cell[i].second);
    }

    // Normalised, because Rack saves absolute grid coordinates around an
    // offset of its own -- a real patch has positions in the thousands, and
    // laying those out literally puts the rack off the side of the world.
    int rows = 1;
    for (std::size_t i = 0; i < modules.size(); ++i) {
        cell[i].first -= min_x;
        cell[i].second -= min_y;
        rows = std::max(rows, cell[i].second + 1);
    }

    // Two panels cannot occupy the same HP: a rack is a rail, and a module
    // screwed to it takes up the width it is.
    //
    // Stored positions overlap whenever whoever wrote them did not know a
    // module's true width -- a patch that spaced five modules 8 HP apart and
    // then found them to be 15, 30, 14, 15 and 24 HP has every one of them
    // sitting inside its neighbour. Drawing that literally stacks the panels
    // on top of each other, so each row is walked left to right and anything
    // that would start inside the panel before it is moved to just after it.
    // Order and true width are both kept; only the gap that was never there
    // is given up.
    std::vector<std::size_t> order(modules.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) {
                         if (cell[a].second != cell[b].second)
                             return cell[a].second < cell[b].second;
                         return cell[a].first < cell[b].first;
                     });
    int row_of_last = -1, right_edge = 0;
    for (const auto i : order) {
        if (cell[i].second != row_of_last) {
            row_of_last = cell[i].second;
            right_edge = cell[i].first;
        }
        cell[i].first = std::max(cell[i].first, right_edge);
        right_edge = cell[i].first + std::max(1, modules[i].hp);
    }

    float span_x = 0.0f;
    for (std::size_t i = 0; i < modules.size(); ++i)
        span_x = std::max(span_x, static_cast<float>(cell[i].first +
                                                     std::max(1, modules[i].hp)));
    out.total_width = span_x * kHorizontalPitch;
    out.rows = rows;
    if (out.total_width <= 0.0f) return out;

    const float total_height = static_cast<float>(rows) * kPanelHeight;

    // Fit the whole rack, with breathing room, and never blow a small one up
    // past a little over life size -- two modules stretched to fill a wide
    // window stop looking like Eurorack.
    const float fit_w = (viewport_width - 70.0f) / out.total_width;
    const float fit_h = (viewport_height - 110.0f) / total_height;
    out.scale = std::min({fit_w, fit_h, 1.05f});
    if (!(out.scale > 0.0f)) out.scale = 0.01f;   // a degenerate viewport, not a crash

    out.origin_x = (viewport_width - out.total_width * out.scale) / 2.0f;
    out.origin_y = (viewport_height - total_height * out.scale) / 2.0f - 10.0f;

    for (std::size_t i = 0; i < modules.size(); ++i) {
        const auto& m = modules[i];
        PanelBox box;
        box.id = m.id;
        box.x = out.origin_x + static_cast<float>(cell[i].first) *
                                   kHorizontalPitch * out.scale;
        box.y = out.origin_y + static_cast<float>(cell[i].second) *
                                   kPanelHeight * out.scale;
        box.width = static_cast<float>(std::max(1, m.hp)) * kHorizontalPitch * out.scale;
        box.height = kPanelHeight * out.scale;
        box.placed = m.placed;
        box.has_artwork = m.has_artwork;
        box.available = m.available;
        box.controls_measured = m.controls_measured;
        out.panels.push_back(box);
    }
    return out;
}

std::vector<ScrewPoint> screw_points(const PanelBox& panel, float scale) {
    std::vector<ScrewPoint> out;
    if (!(panel.width > 0.0f) || !(panel.height > 0.0f)) return out;

    const float inset_x = kScrewInsetX * scale;
    const float inset_y = kScrewInsetY * scale;
    const float left = panel.x + inset_x;
    const float right = panel.x + panel.width - inset_x;
    // At 3 HP the two columns land on the same point, and a narrower panel
    // would put them the wrong way round. One centred screw is what the
    // artwork draws there, and what a real 3 HP module has.
    const bool one_column = right <= left + 0.5f;
    const float mid = panel.x + panel.width / 2.0f;

    for (const float y : {panel.y + inset_y, panel.y + panel.height - inset_y}) {
        if (one_column) {
            out.push_back({mid, y});
        } else {
            out.push_back({left, y});
            out.push_back({right, y});
        }
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

namespace {

/// Does `text` contain `word` as a whole word, ignoring case?
///
/// Whole-word, because "ENV" is inside "ENVELOPE" and a substring match would
/// light a cable for a word the reader never typed. Module names are short
/// and shouty, which makes accidental containment likely rather than rare.
bool mentions(const std::string& text, const std::string& word) {
    if (word.empty()) return false;
    auto lower = [](std::string v) {
        for (auto& ch : v) ch = static_cast<char>(std::tolower(ch));
        return v;
    };
    const auto hay = lower(text), needle = lower(word);
    for (std::size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + 1)) {
        const bool left = at == 0 || !std::isalnum(hay[at - 1]);
        const std::size_t end = at + needle.size();
        const bool right = end >= hay.size() || !std::isalnum(hay[end]);
        if (left && right) return true;
    }
    return false;
}

}  // namespace

std::optional<std::size_t> cable_for_question(
    const std::string& question, const std::vector<Connection>& cables,
    const std::vector<RackModule>& modules) {
    auto named = [&](const std::string& id) {
        for (const auto& m : modules)
            if (m.id == id)
                return mentions(question, m.display.empty() ? m.name : m.display)
                       || mentions(question, m.name);
        return false;
    };

    std::optional<std::size_t> one_end;
    for (std::size_t i = 0; i < cables.size(); ++i) {
        const bool from = named(cables[i].from_module);
        const bool to = named(cables[i].to_module);
        if (from && to) return i;             // both ends: unambiguous
        if ((from || to) && !one_end) one_end = i;
    }
    return one_end;
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

void cable_point(const CableCurve& c, float t, float& x, float& y) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float u = 1.0f - t;
    x = u * u * c.x1 + 2 * u * t * c.cx + t * t * c.x2;
    y = u * u * c.y1 + 2 * u * t * c.cy + t * t * c.y2;
}

float distance_to_cable(const CableCurve& c, float x, float y) {
    float best = std::numeric_limits<float>::max();
    float px = 0, py = 0;
    cable_point(c, 0.0f, px, py);
    for (int i = 1; i <= kCableSegments; ++i) {
        float qx = 0, qy = 0;
        cable_point(c, static_cast<float>(i) / kCableSegments, qx, qy);
        // Nearest point on this piece, clamped to its ends so the distance is
        // to the segment and not to the infinite line it lies on.
        const float dx = qx - px, dy = qy - py;
        const float len2 = dx * dx + dy * dy;
        float t = 0.0f;
        if (len2 > 0.0f)
            t = std::clamp(((x - px) * dx + (y - py) * dy) / len2, 0.0f, 1.0f);
        const float ex = px + dx * t - x, ey = py + dy * t - y;
        best = std::min(best, std::sqrt(ex * ex + ey * ey));
        px = qx;
        py = qy;
    }
    return best;
}

}  // namespace forge_modular
