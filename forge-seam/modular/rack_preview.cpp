#include "forge/rack_preview.hpp"

#include <forge/design_tokens.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

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
    // The same flattening the hit test uses, so the cable a pointer finds is
    // the cable that was drawn.
    Canvas::Point2D pts[kCableSegments + 1];
    for (int i = 0; i <= kCableSegments; ++i) {
        cable_point(c, static_cast<float>(i) / kCableSegments, pts[i].x, pts[i].y);
        pts[i].y += dy;
    }
    canvas.set_stroke_color(stroke);
    canvas.set_line_width(width);
    canvas.set_line_cap(pulp::canvas::LineCap::round);
    canvas.stroke_path(pts, kCableSegments + 1);
}

/// The largest size at or below `wanted` that fits `text` into `room`.
///
/// A long slug on a narrow panel -- "AudioInterface2" on 5 HP -- has to give
/// somewhere. Set smaller it is still the module's name; clipped it is a
/// different word, and centred-and-unclipped it labels the module next door.
/// Floored rather than shrunk without limit, because a name too small to read
/// has stopped naming anything; the clip behind it catches that case.
float fitted_font_size(Canvas& canvas, const char* family,
                       const std::string& text, float wanted, float room) {
    if (text.empty() || !(room > 0.0f)) return wanted;
    canvas.set_font(family, wanted);
    const float width = canvas.measure_text(text);
    if (!(width > room)) return wanted;
    return std::max(wanted * 0.62f, wanted * room / width);
}

/// Make one panel's boundary legible where it butts against the next.
///
/// A real rack has no gutters, and the layout is right not to invent any -- but
/// a row of panels with no seam reads as one undifferentiated strip, and a rack
/// you cannot count the modules in cannot be checked against what was asked
/// for. The boundary is drawn rather than spaced: shading that falls off into
/// each panel's own face, and a bright/dark hairline pair that meets at the
/// join the way two machined edges do.
void draw_panel_edges(Canvas& canvas, const PanelBox& panel) {
    canvas.save();
    canvas.clip_rect(panel.x, panel.y, panel.width, panel.height);

    // Shading into both sides. Falls off within a tenth of the panel, so a wide
    // panel is not darkened across its face and a narrow one is not swallowed.
    const Color stops[] = {
        Color::rgba8(0, 0, 0, 86), Color::rgba8(0, 0, 0, 0),
        Color::rgba8(0, 0, 0, 0), Color::rgba8(0, 0, 0, 86),
    };
    const float at[] = {0.0f, 0.11f, 0.89f, 1.0f};
    canvas.set_fill_gradient_linear(panel.x, panel.y, panel.x + panel.width,
                                    panel.y, stops, at, 4);
    canvas.fill_rect(panel.x, panel.y, panel.width, panel.height);
    canvas.clear_fill_gradient();

    // The seam itself. Light down the left edge, dark down the right, so the
    // join between two panels is a lit edge beside a shadowed one -- visible
    // even where both faces happen to be the same colour, which is exactly the
    // case a shading gradient alone cannot separate.
    canvas.set_line_width(1.0f);
    canvas.set_stroke_color(Color::rgba8(255, 255, 255, 40));
    canvas.stroke_line(panel.x + 0.5f, panel.y, panel.x + 0.5f,
                       panel.y + panel.height);
    canvas.set_stroke_color(Color::rgba8(0, 0, 0, 150));
    canvas.stroke_line(panel.x + panel.width - 0.5f, panel.y,
                       panel.x + panel.width - 0.5f, panel.y + panel.height);
    // A lit top edge, the way a panel catches the light off the rail above it.
    canvas.set_stroke_color(Color::rgba8(255, 255, 255, 33));
    canvas.stroke_line(panel.x, panel.y + 0.5f, panel.x + panel.width,
                       panel.y + 0.5f);
    canvas.restore();
}

/// The rail screws, over whatever face was drawn.
///
/// The artwork draws its screw HOLES -- a hollow ring a third of a point wide,
/// which at preview scale is a grey smudge and at any scale is flat. Painting
/// the screw here gives it a head, a rim and a slot, and gives one to the
/// third-party panels that have no artwork of their own, so a rack does not
/// have screws on some modules and not others.
void draw_screws(Canvas& canvas, const PanelBox& panel, float scale) {
    // Never smaller than a couple of points: below that a head, a rim and a
    // slot land on the same pixel and the screw reads as dirt.
    const float r = std::max(2.2f, kScrewRadius * scale);
    canvas.save();
    canvas.clip_rect(panel.x, panel.y, panel.width, panel.height);
    for (const auto& s : screw_points(panel, scale)) {
        // Seated in the panel: a shadow under the head before the head itself.
        canvas.set_fill_color(Color::rgba8(0, 0, 0, 110));
        canvas.fill_circle(s.x, s.y + r * 0.16f, r);
        // A domed head, lit from above and to the left the way the rest of the
        // chrome is. Graded rather than a lighter disc laid on a darker one:
        // the join between two flat circles is itself a circle, and the screw
        // reads as a crescent moon instead of a piece of hardware.
        const Color head[] = {
            Color::rgba8(0x8C, 0x95, 0xA1), Color::rgba8(0x5C, 0x65, 0x71),
            Color::rgba8(0x31, 0x37, 0x3F),
        };
        const float at[] = {0.0f, 0.6f, 1.0f};
        canvas.set_fill_gradient_radial_two_circles(
            s.x - r * 0.34f, s.y - r * 0.34f, r * 0.1f, s.x, s.y, r * 1.04f,
            head, at, 3);
        canvas.fill_circle(s.x, s.y, r);
        canvas.clear_fill_gradient();
        canvas.set_stroke_color(Color::rgba8(0x14, 0x18, 0x1E, 170));
        canvas.set_line_width(std::max(0.7f, r * 0.16f));
        canvas.stroke_circle(s.x, s.y, r);
        // The slot. Straight across: a driver leaves it wherever it stopped,
        // but a preview that angled each one differently would flicker as the
        // rack relaid out, and a screw that moves is worse than a tidy one.
        canvas.set_stroke_color(Color::rgba8(0x0C, 0x0F, 0x14, 200));
        canvas.set_line_width(std::max(0.9f, r * 0.2f));
        canvas.set_line_cap(pulp::canvas::LineCap::butt);
        canvas.stroke_line(s.x - r * 0.52f, s.y, s.x + r * 0.52f, s.y);
    }
    canvas.restore();
}

/// A jack socket, for a panel that has no artwork drawing its own.
///
/// Without this a cable on a plain face ends on a featureless slab: the ring
/// alone reads as a cable stopping in mid-air rather than as a lead going into
/// something. Reported as the audio interface "missing connections" when in
/// fact every cable reached it -- there was simply nothing there to arrive at.
void draw_socket(Canvas& canvas, float cx, float cy, float r, Color plug) {
    canvas.set_fill_color(Color::rgba8(0, 0, 0, 120));
    canvas.fill_circle(cx, cy + r * 0.14f, r);
    canvas.set_fill_color(Color::rgba8(0x77, 0x80, 0x8B));      // the nut
    canvas.fill_circle(cx, cy, r);
    canvas.set_fill_color(Color::rgba8(0x9C, 0xA5, 0xB0, 190));
    canvas.fill_circle(cx - r * 0.2f, cy - r * 0.24f, r * 0.66f);
    canvas.set_stroke_color(Color::rgba8(0, 0, 0, 150));
    canvas.set_line_width(std::max(0.7f, r * 0.16f));
    canvas.stroke_circle(cx, cy, r);
    canvas.set_fill_color(Color::rgba8(0x0B, 0x0E, 0x12));      // the bore
    canvas.fill_circle(cx, cy, r * 0.56f);
    // The plug filling the bore, in the cable's own colour, so the socket says
    // which lead is in it rather than merely that one is.
    canvas.set_fill_color(plug);
    canvas.fill_circle(cx, cy, r * 0.42f);
}

/// The end of a cable whose jack position was never captured.
///
/// Docked at the panel edge because guessing a jack centre would look
/// authoritative and be wrong. That honesty cost the drawing, though: the cable
/// simply stopped, which reads as a connection that is not there. A collar in
/// the cable's colour makes it a lead entering the panel from the side -- still
/// visibly not a jack, and visibly a connection.
void draw_dock_collar(Canvas& canvas, const JackPoint& p, Color body, float scale) {
    const float w = std::max(4.0f, 8.0f * scale);
    const float h = std::max(9.0f, 18.0f * scale);
    canvas.set_fill_color(body);
    canvas.fill_rounded_rect(p.x - w / 2.0f, p.y - h / 2.0f, w, h, w / 2.0f);
    // Tied to the cable's own alpha, so a dimmed cable's collar dims with it.
    canvas.set_stroke_color(Color{1.0f, 1.0f, 1.0f, body.a * 0.28f});
    canvas.set_line_width(1.0f);
    canvas.stroke_rounded_rect(p.x - w / 2.0f, p.y - h / 2.0f, w, h, w / 2.0f);
}

}  // namespace

const std::string& RackPreview::panel_svg(const std::string& slug) const {
    static const std::string kNone;
    if (slug.empty()) return kNone;
    const auto it = panel_cache_.find(slug);
    if (it != panel_cache_.end()) return it->second;
    std::string text;
    if (!panel_dir_.empty()) text = read_panel(panel_dir_, slug);
    return panel_cache_.emplace(slug, std::move(text)).first->second;
}

std::string RackPreview::read_panel(const std::string& dir,
                                    const std::string& slug) {
    // LIGHT first, because that is what Rack shows.
    //
    // This preferred `-dark.svg`, on the reasoning that a light panel on a
    // dark stage reads as a mistake. It reads as a mistake for a better
    // reason: the same rack is white in Rack and black here, so the preview
    // and the thing it previews disagree about what the user is about to see.
    // Fidelity beats fitting in -- the picture's whole job is to be the rack
    // you get.
    for (const char* suffix : {".svg", "-dark.svg"}) {
        std::ifstream f(dir + "/" + slug + suffix, std::ios::binary);
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }
    return {};
}

const std::string& RackPreview::vendor_svg(const std::string& plugin,
                                           const std::string& model) const {
    // Somebody else's module, drawn from THEIR artwork.
    //
    // Only our own panels were ever looked up, so every module we did not make
    // came out as a blank slab -- most visibly Core's audio interface, which
    // is in every patch that makes a sound and was the emptiest thing on the
    // stage. Its cables then docked at the panel edge rather than at jacks,
    // because a face with no artwork has no jack positions, which is the odd
    // wiring that goes with it.
    //
    // Rack keeps Core's artwork inside its own bundle and every other
    // plugin's beside the plugin, so both are readable without shipping
    // copies of anyone's work.
    static const std::string kNone;
    const auto key = plugin + "/" + model;
    const auto it = vendor_cache_.find(key);
    if (it != vendor_cache_.end()) return it->second;

    std::vector<std::pair<std::string, std::string>> tries;
    if (plugin == "Core") {
        // Core's files are named for what the module IS rather than for its
        // slug: AudioInterface2 is Audio2.svg, AudioInterface16 is Audio8.svg
        // in the free build. Slug first, then the shortened name.
        std::string shortened = model;
        const std::string prefix = "AudioInterface";
        if (model.rfind(prefix, 0) == 0)
            shortened = "Audio" + model.substr(prefix.size());
        for (const char* app : {"/Applications/VCV Rack 2 Free.app",
                                "/Applications/VCV Rack 2 Pro.app"}) {
            const std::string dir = std::string(app) + "/Contents/Resources/res/Core";
            tries.emplace_back(dir, model);
            if (shortened != model) tries.emplace_back(dir, shortened);
        }
    } else {
        const char* home = std::getenv("HOME");
        const std::string base = std::string(home ? home : ".") +
                                 "/Library/Application Support/Rack2";
        for (const char* arch : {"plugins-mac-arm64", "plugins-mac-x64", "plugins"})
            tries.emplace_back(base + "/" + arch + "/" + plugin + "/res", model);
    }

    std::string text;
    for (const auto& [dir, name] : tries) {
        text = read_panel(dir, name);
        if (!text.empty()) break;
    }
    return vendor_cache_.emplace(key, std::move(text)).first->second;
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

std::optional<std::size_t> RackPreview::cable_at(float x, float y) const {
    const auto b = bounds();
    const auto L = layout_rack(modules_, b.width, b.height);
    std::optional<std::size_t> best;
    float best_d = kCableGrabPoints;
    for (std::size_t i = 0; i < connections_.size(); ++i) {
        const auto& c = connections_[i];
        const auto from = port_point(L, modules_, c.from_module, c.from_port,
                                     c.to_module);
        const auto to = port_point(L, modules_, c.to_module, c.to_port,
                                   c.from_module);
        // progress_, not 1: a cable that is only half drawn can only be
        // pointed at where it has actually been drawn.
        const float d = distance_to_cable(cable_curve(from, to, progress_), x, y);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

void RackPreview::on_hover_move(pulp::view::Point local_pos) {
    const auto found = cable_at(static_cast<float>(local_pos.x),
                                static_cast<float>(local_pos.y));
    if (found == highlight_) return;
    set_highlight(found);
    if (on_cable_hover) on_cable_hover(found);
}

void RackPreview::on_mouse_leave() {
    // The pointer leaving the rack has to clear BOTH ends, or a line stays lit
    // in the explanation pointing at a cable nobody is near.
    if (!highlight_) return;
    set_highlight(std::nullopt);
    if (on_cable_hover) on_cable_hover(std::nullopt);
}

void RackPreview::paint(Canvas& canvas) {
    const auto b = bounds();
    const auto L = layout_rack(modules_, b.width, b.height);

    /// Panels drawn without artwork of their own, so nothing on them says where
    /// a jack is. Collected here and used by the cable pass below.
    std::set<std::string> plain_faces;

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
        // Ours from our own res/, anyone else's from theirs. Matching on the
        // slug alone would draw OUR panel on a vendor module -- Fundamental
        // also ships VCO, VCF, VCA and LFO -- which looks plausible, has the
        // wrong controls and the wrong width, and is a confident lie about
        // what is in the rack.
        const std::string& svg =
            !mod_for_panel                        ? panel_svg(std::string{})
            : mod_for_panel->brand == kOurs       ? panel_svg(mod_for_panel->name)
            : vendor_svg(mod_for_panel->brand, mod_for_panel->name);
        bool artwork = false;
        if (!svg.empty()) {
            // Clipped to its own slot. Each panel's artwork paints a
            // background rect a millimetre PROUD of its viewBox on every side
            // -- deliberate, so a panel has no hairline seam when Rack butts it
            // against its neighbour -- and unclipped here that overhang draws
            // straight over the modules either side.
            canvas.save();
            canvas.clip_rect(panel.x, panel.y, panel.width, panel.height);
            artwork = canvas.draw_svg(svg, panel.x, panel.y, panel.width,
                                      panel.height);
            canvas.restore();
        }
        // The real face, knobs and all: nothing else to draw for it. A panel
        // with no artwork of its own has no jacks either, so its id is kept --
        // the cable pass draws a socket where each lead lands, or the cable
        // arrives on a blank slab and the module reads as unconnected.
        if (!artwork) plain_faces.insert(panel.id);
        if (artwork) continue;

        // No artwork for this one. Drawn as a HATCHED face at its true width
        // rather than a plain rectangle, because a plain rectangle is exactly
        // what a broken panel looks like -- and a reader who cannot tell "we
        // have no picture of this module" from "this module is empty" will
        // read a working rack as a failure, or worse, the reverse.
        // Hatched only when artwork was EXPECTED and is not there: one of ours
        // whose panel failed to emit or failed to parse. A third-party module
        // we never had a picture of is drawn plainly, because that is not a
        // fault and hatching every one of them would bury the case that is.
        const auto* mod_here = find(panel.id);
        const bool ours = mod_here && mod_here->brand == kOurs;
        const bool undrawn = ours || !panel.has_artwork;
        canvas.set_fill_color(undrawn ? color::surface_sunken
                                      : color::surface_panel);
        canvas.fill_rounded_rect(panel.x, panel.y, panel.width, panel.height,
                                 4.0f * L.scale);
        if (undrawn) {
            canvas.save();
            canvas.clip_rect(panel.x, panel.y, panel.width, panel.height);
            canvas.set_stroke_color(color::line);
            canvas.set_line_width(1.0f);
            const float step = 9.0f * L.scale;
            for (float x = panel.x - panel.height; x < panel.x + panel.width;
                 x += step)
                canvas.stroke_line(x, panel.y + panel.height,
                                   x + panel.height, panel.y);
            canvas.restore();
        }
        canvas.set_stroke_color(color::line);
        canvas.set_line_width(1.0f);
        canvas.stroke_rounded_rect(panel.x, panel.y, panel.width, panel.height,
                                   4.0f * L.scale);

        // Name the panel. A rack of anonymous rectangles cannot be checked
        // against what was asked for, which is most of what a preview is for.
        //
        // Clipped to its own panel. A long slug -- "AudioInterface2" on a 5 HP
        // module -- is centred, so it runs out over both neighbours and labels
        // modules it does not name. Truncated is honest; spilling is not.
        const auto* mod = find(panel.id);
        if (!mod) continue;
        canvas.save();
        canvas.clip_rect(panel.x, panel.y, panel.width, panel.height);
        canvas.set_fill_color(color::text);
        canvas.set_text_align(pulp::canvas::TextAlign::center);
        canvas.set_font(forge::design::type::display,
                        fitted_font_size(canvas, forge::design::type::display,
                                         mod->name, 12.0f * L.scale,
                                         panel.width - 6.0f * L.scale));
        canvas.fill_text(mod->name, panel.x + panel.width / 2.0f,
                         panel.y + 22.0f * L.scale);
        canvas.set_font(forge::design::type::mono, 8.5f * L.scale);
        canvas.set_fill_color(color::text_faint);
        canvas.fill_text(undrawn ? "NO PANEL" : mod->brand,
                         panel.x + panel.width / 2.0f,
                         panel.y + panel.height - 12.0f * L.scale);
        canvas.restore();
    }

    // The seams and the rail screws, over every face alike.
    //
    // A second pass rather than part of the one above, because the artwork
    // paints a background a millimetre proud of its own viewBox on all four
    // sides -- so a panel drawn after its neighbour would paint over the
    // neighbour's seam, and the strip would have a boundary everywhere except
    // wherever the loop happened to have got to last.
    for (const auto& panel : L.panels) {
        draw_panel_edges(canvas, panel);
        draw_screws(canvas, panel, L.scale);
    }

    // A module Rack cannot create says so, over whatever was drawn for it.
    //
    // The preview reads OUR manifests and can draw a beautiful panel for a
    // module the installed plugin does not contain -- which then opens in Rack
    // as a gap. Reported as "the VCV Rack patch/models are DIFFERENT than what
    // I see in Forge Modular". A panel that will not be there has to look like
    // one, or the preview is a confident lie about what you will get.
    for (const auto& panel : L.panels) {
        if (panel.available) continue;
        canvas.save();
        canvas.clip_rect(panel.x, panel.y, panel.width, panel.height);
        canvas.set_fill_color(pulp::canvas::Color::rgba8(0, 0, 0, 190));
        canvas.fill_rect(panel.x, panel.y, panel.width, panel.height);
        // Full alpha: a blended stroke is a different colour, and this mark
        // has to be unmistakable both to a reader and to the test that pins it.
        canvas.set_stroke_color(pulp::canvas::Color::rgba8(0xF3, 0x37, 0x4B));
        canvas.set_line_width(2.5f);
        canvas.stroke_rounded_rect(panel.x + 1.0f, panel.y + 1.0f,
                                   panel.width - 2.0f, panel.height - 2.0f,
                                   4.0f * L.scale);
        canvas.set_font(forge::design::type::mono,
                        std::max(9.0f, 10.0f * L.scale));
        canvas.set_fill_color(pulp::canvas::Color::rgba8(0xF3, 0x37, 0x4B));
        canvas.set_text_align(pulp::canvas::TextAlign::center);
        canvas.fill_text("NOT IN RACK", panel.x + panel.width / 2.0f,
                         panel.y + panel.height / 2.0f);
        canvas.set_text_align(pulp::canvas::TextAlign::left);
        canvas.restore();
    }

    // What each colour means, bottom right, and only for the roles this patch
    // actually uses. A legend listing four roles for a patch that has two says
    // something untrue about the rack in front of you.
    {
        struct Entry { SignalRole role; const char* title; };
        static constexpr Entry kEntries[] = {
            {SignalRole::audio, "AUDIO"},
            {SignalRole::pitch, "PITCH & GATE"},
            {SignalRole::clock, "CLOCK"},
            {SignalRole::mod,   "MODULATION"},
        };
        float x = bounds().width - 12.0f;
        const float y = bounds().height - 14.0f;
        canvas.set_font(forge::design::type::mono, 9.0f);
        canvas.set_text_align(pulp::canvas::TextAlign::right);
        // Right to left, so the rightmost entry sits against the margin
        // whatever the patch happens to contain.
        for (int i = 3; i >= 0; --i) {
            const auto& e = kEntries[i];
            bool used = false;
            for (const auto& c : connections_)
                if (c.role == e.role) { used = true; break; }
            if (!used) continue;
            canvas.set_fill_color(color::text_faint);
            canvas.fill_text(e.title, x, y);
            x -= static_cast<float>(std::strlen(e.title)) * 5.4f + 10.0f;
            const auto rgb = role_color(e.role);
            canvas.set_fill_color(pulp::canvas::Color::rgba8(
                static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
                static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
                static_cast<std::uint8_t>(rgb & 0xFF)));
            canvas.fill_circle(x + 2.0f, y - 3.0f, 3.5f);
            x -= 14.0f;
        }
        canvas.set_text_align(pulp::canvas::TextAlign::left);
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

        // Both ends, so a cable arrives at something rather than stopping.
        const std::pair<const JackPoint&, const std::string&> ends[] = {
            {from, c.from_module}, {to, c.to_module},
        };
        for (const auto& [p, module_id] : ends) {
            if (p.docked) {
                // No captured coordinates, so no jack to draw -- a collar in
                // the cable's colour instead, entering the panel from the side.
                draw_dock_collar(canvas, p, from_rgb(rgb, a), L.scale);
                continue;
            }
            // A panel with artwork draws its own jacks; one without draws
            // nothing, and a lead landing on a blank face reads as a module
            // with no connections at all. The socket is what the cable arrives
            // at in that case.
            const float r = std::max(3.4f, kJackRadius * L.scale);
            if (plain_faces.count(module_id))
                draw_socket(canvas, p.x, p.y, r, from_rgb(rgb, a));
            // The same ring at both kinds of end, outside the rim rather than
            // lost down the bore. A ring sized to the drawn socket on one
            // module and to nothing in particular on the next makes two
            // identical connections look like different things.
            canvas.set_stroke_color(from_rgb(rgb, a));
            canvas.set_line_width(1.5f);
            canvas.stroke_circle(p.x, p.y, r + 3.0f * L.scale);
        }
    }
}

}  // namespace forge_modular
