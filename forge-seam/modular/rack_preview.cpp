#include "forge/rack_preview.hpp"

#include <choc/text/choc_JSON.h>

#include "forge/portmap.hpp"

#include <forge/design_tokens.hpp>

#include <pulp/canvas/svg_dom_cache.hpp>

#include <algorithm>
#include <cctype>
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

/// One attribute off an XML start tag, as a number, ignoring any unit suffix.
/// Zero when the attribute is not there or does not begin with a number.
float tag_number(const std::string& tag, const std::string& attr) {
    // `attr="` rather than `attr`, so `width` does not match `stroke-width`
    // and `height` does not match a `height`-suffixed name either. Searched
    // from the start of the tag with the quote included because an SVG root
    // carries several attributes whose names end in the ones wanted.
    for (std::size_t at = tag.find(attr); at != std::string::npos;
         at = tag.find(attr, at + 1)) {
        // A real attribute name begins the token: the character before it is
        // whitespace or the tag name's own space.
        if (at > 0 && !std::isspace(static_cast<unsigned char>(tag[at - 1])))
            continue;
        std::size_t p = at + attr.size();
        while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p]))) ++p;
        if (p >= tag.size() || tag[p] != '=') continue;
        ++p;
        while (p < tag.size() && (std::isspace(static_cast<unsigned char>(tag[p])) ||
                                  tag[p] == '"' || tag[p] == '\'')) ++p;
        try {
            std::size_t used = 0;
            const float v = std::stof(tag.substr(p, 32), &used);
            if (used > 0) return v;
        } catch (...) {
            // Not a number. Keep looking rather than giving up: an SVG root
            // may carry the same suffix twice.
        }
    }
    return 0.0f;
}

}  // namespace

int panel_hp_from_artwork(const std::string& svg) {
    const auto open = svg.find("<svg");
    if (open == std::string::npos) return 0;
    const auto close = svg.find('>', open);
    if (close == std::string::npos) return 0;
    const std::string tag = svg.substr(open, close - open);

    // The viewBox first, because its two numbers are certainly in the same
    // units as each other. `width` and `height` normally are too -- both in
    // millimetres, or both bare -- but they are two attributes and can differ,
    // and a width in millimetres divided by a height in points is a number
    // with no meaning that still looks like a plausible module.
    float w = 0.0f, h = 0.0f;
    const auto vb = tag.find("viewBox");
    if (vb != std::string::npos) {
        const auto q = tag.find_first_of("\"'", vb);
        if (q != std::string::npos) {
            std::istringstream in(tag.substr(q + 1, 96));
            float x = 0, y = 0;
            in >> x >> y >> w >> h;
            if (!in) { w = 0.0f; h = 0.0f; }
        }
    }
    if (!(w > 0.0f) || !(h > 0.0f)) {
        w = tag_number(tag, "width");
        h = tag_number(tag, "height");
    }
    if (!(w > 0.0f) || !(h > 0.0f)) return 0;

    // Only the aspect is used, and the height is a constant of the format --
    // every 3U panel is 128.5 mm tall, whatever units it was drawn in.
    const float hp = (w / h) * (kPanelHeightMm / kHorizontalPitchMm);
    const int rounded = static_cast<int>(std::lround(hp));
    // A rack rail is 84 HP and the widest module anyone ships is well inside
    // that. Anything outside this is not a Eurorack panel -- an icon, a logo,
    // a file that happens to be beside the panels -- and guessing from it
    // would be worse than the fallback it replaced.
    if (rounded < 1 || rounded > 200) return 0;
    return rounded;
}

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

const std::vector<RackPreview::KnobSpec>&
RackPreview::module_knobs(const std::string& model) const {
    static const std::vector<KnobSpec> kNone;
    const auto it = knob_cache_.find(model);
    if (it != knob_cache_.end()) return it->second;

    // The manifest the panel was emitted from, beside the artwork it emitted.
    // Reading it rather than the panel's hidden components group, because that
    // group records WHERE each control is and not WHICH it is -- and a trimpot
    // drawn at knob size is a worse lie than no knob at all.
    std::vector<KnobSpec> out;
    if (!panel_dir_.empty()) {
        std::string slug = model;
        for (auto& ch : slug) ch = static_cast<char>(std::tolower(ch));
        const auto path = std::filesystem::path(panel_dir_).parent_path() /
                          "modules" / (slug + ".json");
        std::ifstream f(path);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            try {
                const auto doc = choc::json::parse(ss.str());
                // Diameters as components.py draws them. A kind we do not
                // know is skipped rather than guessed: a switch or a slider
                // drawn as a knob is wrong in a way a reader cannot see.
                const auto diameter = [](const std::string& kind) {
                    if (kind == "KnobLarge") return 18.3f;
                    if (kind == "Knob")      return 12.2f;
                    if (kind == "KnobSmall") return 8.64f;
                    if (kind == "Trimpot")   return 5.76f;
                    return 0.0f;
                };
                if (doc.hasObjectMember("modules")) {
                    const auto mods = doc["modules"];
                    for (uint32_t i = 0; i < mods.size(); ++i) {
                        const auto m = mods[i];
                        if (m.hasObjectMember("params")) {
                            const auto ps = m["params"];
                            for (uint32_t j = 0; j < ps.size(); ++j) {
                                const auto q = ps[j];
                                const float d = diameter(
                                    q["kind"].getWithDefault<std::string>("Knob"));
                                if (d <= 0.0f) continue;
                                out.push_back({
                                    static_cast<float>(
                                        q["x_mm"].getWithDefault<double>(0.0)),
                                    static_cast<float>(
                                        q["y_mm"].getWithDefault<double>(0.0)),
                                    d});
                            }
                        }
                        // Repeated controls are ONE entry with a count and a
                        // grid, not N entries in `params`. Reading only
                        // `params` is why the six-channel mixer drew a single
                        // knob -- its master -- and no channel strip at all:
                        // the manifest recorded all seven, and six of them
                        // were in a shape this never looked at.
                        if (!m.hasObjectMember("param_array")) continue;
                        const auto as = m["param_array"];
                        for (uint32_t a = 0; a < as.size(); ++a) {
                            const auto arr = as[a];
                            const auto count =
                                arr["count"].getWithDefault<int64_t>(0);
                            if (count <= 0 || !arr.hasObjectMember("grid"))
                                continue;
                            float d = 12.2f;
                            if (arr.hasObjectMember("template"))
                                d = diameter(arr["template"]["kind"]
                                                 .getWithDefault<std::string>("Knob"));
                            if (d <= 0.0f) continue;
                            const auto g = arr["grid"];
                            const auto cols =
                                std::max<int64_t>(1, g["cols"].getWithDefault<int64_t>(1));
                            const auto x0 = g["x0_mm"].getWithDefault<double>(0.0);
                            const auto y0 = g["y0_mm"].getWithDefault<double>(0.0);
                            const auto dx = g["dx_mm"].getWithDefault<double>(0.0);
                            const auto dy = g["dy_mm"].getWithDefault<double>(0.0);
                            for (int64_t n = 0; n < count; ++n)
                                out.push_back({
                                    static_cast<float>(x0 + (n % cols) * dx),
                                    static_cast<float>(y0 + (n / cols) * dy),
                                    d});
                        }
                    }
                }
            } catch (...) {
                // A manifest we cannot read means no knobs drawn, not a crash
                // and not a wrong panel.
                out.clear();
            }
        }
    }
    return knob_cache_.emplace(model, std::move(out)).first->second;
}

/// Screens and lamps: measured by the scan, drawn by nobody until now.
///
/// CARTOG has always recorded a module's `displays` (a rectangle plus the
/// widget's class name) and `lights` (a rectangle), because a panel is more
/// than its knobs and jacks. The preview drew neither, so a Quantizer came out
/// as a bare strip where Rack shows a touch plate of twelve key plates, and the
/// audio interface lost its meter ladder entirely — the two things a person
/// looks at to know the patch is alive.
///
/// Drawn as what they ARE, generically: a recessed screen and a lamp. This does
/// not reproduce a vendor's artwork and should not try — the point is that the
/// panel reads as the same instrument, with something where the something is.
/// The class name is used only to tell a KEY PLATE from a plain screen, because
/// a keyboard drawn as a blank rectangle loses the one thing that makes it
/// recognisable.
void RackPreview::draw_screens(pulp::canvas::Canvas& canvas, const PanelBox& panel,
                               const RackModule& mod, float scale) const {
    const auto* m = PortMap::shared().find(mod.brand, mod.name);
    if (!m) return;

    // A widget drawn INSIDE another measured widget is already represented.
    //
    // The scan records a Quantizer's touch plate and, separately, each of its
    // twelve key buttons — thirteen displays for one control. Drawing all of
    // them put twelve recessed rectangles on top of the plate the keys were
    // already painted on, so the panel read as hatching. Containment is the
    // test, not the class name: it is true of any module whose author nested
    // widgets, and false for two screens that merely sit near each other.
    auto nested_in_another = [&](const MappedWidget& d) {
        for (const auto& other : m->displays) {
            if (&other == &d) continue;
            if (!(other.w > d.w) && !(other.h > d.h)) continue;   // not larger
            if (d.x - d.w / 2 >= other.x - other.w / 2 - 0.5f &&
                d.x + d.w / 2 <= other.x + other.w / 2 + 0.5f &&
                d.y - d.h / 2 >= other.y - other.h / 2 - 0.5f &&
                d.y + d.h / 2 <= other.y + other.h / 2 + 0.5f)
                return true;
        }
        return false;
    };

    for (const auto& d : m->displays) {
        if (!(d.w > 0.0f) || !(d.h > 0.0f)) continue;
        if (nested_in_another(d)) continue;
        const float w = d.w * scale, h = d.h * scale;
        if (w < 3.0f || h < 3.0f) continue;         // below this it is a speck
        const float x = panel.x + d.x * scale - w / 2.0f;
        const float y = panel.y + d.y * scale - h / 2.0f;
        // Recessed, like a window cut into the panel.
        canvas.set_fill_color(from_rgb(0x0E1218, 1.0f));
        canvas.fill_rounded_rect(x, y, w, h, std::min(3.0f * scale, h * 0.2f));
        canvas.set_stroke_color(from_rgb(0x2A323D, 1.0f));
        canvas.set_line_width(std::max(0.5f, 0.8f * scale));
        canvas.stroke_rounded_rect(x, y, w, h, std::min(3.0f * scale, h * 0.2f));

        // A touch plate is a stack of keys. Rack's quantizer names its widget
        // QuantizerDisplay and puts twelve QuantizerButtons on it; anything
        // whose name says keyboard or quantizer gets the same treatment, so
        // the plate reads as a plate rather than as a blank window.
        std::string t = d.type;
        for (auto& ch : t) ch = static_cast<char>(std::tolower(
            static_cast<unsigned char>(ch)));
        const bool is_plate = t.find("quantizer") != std::string::npos ||
                              t.find("keyboard") != std::string::npos ||
                              t.find("piano") != std::string::npos;
        if (is_plate && h > 12.0f * scale) {
            // Drawn as a KEYBOARD, not as stripes.
            //
            // The first version alternated full-width light and dark bars,
            // twelve of them, which at panel scale reads as hatching rather
            // than as keys. A piano is white keys across the full width with
            // the black ones narrower and sitting BETWEEN them — that overlap
            // is the whole visual signature, and losing it loses the only
            // thing that says "this selects notes".
            static const bool sharp[12] = {false, true, false, true, false,
                                           false, true, false, true, false,
                                           true, false};
            // In Rack's own colours, which is the point of drawing it at all.
            //
            // A pale piano was the first attempt, and at preview scale a
            // light strip on a light panel is the one thing it must not be:
            // it disappears into the faceplate and reads as an empty window,
            // which is exactly what a touch plate looked like before it was
            // drawn. Rack lights this plate amber on near-black, so the
            // module is identifiable at a glance across the rack -- the same
            // reason its cables are coloured.
            canvas.set_fill_color(from_rgb(0x0C0E11, 1.0f));
            canvas.fill_rect(x, y, w, h);
            // Seven natural keys to the octave, laid out first.
            const float wh = h / 7.0f;
            for (int i = 0; i < 7; ++i) {
                const float ky = y + static_cast<float>(i) * wh;
                canvas.set_fill_color(from_rgb(0xC9971F, 1.0f));
                canvas.fill_rect(x + w * 0.08f, ky + 0.6f * scale,
                                 w * 0.84f, wh - 1.2f * scale);
            }
            // Then the sharps, over the joins and darker, so the overlap that
            // makes a keyboard legible survives.
            const float bh = h / 12.0f;
            for (int i = 0; i < 12; ++i) {
                if (!sharp[11 - i]) continue;
                const float ky = y + static_cast<float>(i) * bh;
                canvas.set_fill_color(from_rgb(0x0C0E11, 1.0f));
                canvas.fill_rect(x + w * 0.08f, ky + bh * 0.14f,
                                 w * 0.50f, bh * 0.72f);
            }
        }
    }

    // Lamps. Level meters are a column of these, which is why an audio
    // interface with none looks switched off.
    for (const auto& l : m->lights) {
        if (!(l.w > 0.0f)) continue;
        const float r = l.w * scale / 2.0f;
        if (!(r > 0.4f)) continue;
        const float cx = panel.x + l.x * scale;
        const float cy = panel.y + l.y * scale;
        // Unlit, but VISIBLE. The first version drew a near-black disc with a
        // near-black ring, which at panel scale disappeared into the panel —
        // an audio interface rendered as if it had no meter at all, which is
        // the very thing this was added to fix. A lamp that is off still has a
        // lens.
        canvas.set_fill_color(from_rgb(0x0B0E13, 1.0f));
        canvas.fill_circle(cx, cy, r);
        canvas.set_stroke_color(from_rgb(0x6E7B8C, 1.0f));
        canvas.set_line_width(std::max(0.6f, r * 0.30f));
        canvas.stroke_circle(cx, cy, r * 0.78f);
    }
}

void RackPreview::draw_jacks(pulp::canvas::Canvas& canvas, const PanelBox& panel,
                             const RackModule& mod, float scale) const {
    const auto* m = PortMap::shared().find(mod.brand, mod.name);
    if (!m) return;

    // Every jack the module has, not only the ones with a cable in them.
    //
    // Sockets used to be drawn by the cable pass, and only for panels with no
    // artwork of their own -- the reasoning being that a panel WITH artwork
    // draws its own. It does not. A panel SVG is a background; Rack composites
    // the jacks on top of it, exactly as it composites the knobs. So a module
    // with artwork showed a jack only where a lead happened to land, and VCA
    // -- three jacks, one patched -- came out with two holes missing.
    //
    // Drawn here, before the cables, so a lead still lands on a plug in its
    // own colour: the cable pass paints over the ones it uses.
    const float r = std::max(2.6f, kJackRadius * scale);
    for (const auto* group : {&m->inputs, &m->outputs})
        for (const auto& j : *group)
            draw_socket(canvas, panel.x + j.x * scale, panel.y + j.y * scale, r,
                        from_rgb(0x0B0E12, 1.0f));
}

void RackPreview::draw_knobs(pulp::canvas::Canvas& canvas, const PanelBox& panel,
                             const RackModule& mod, float scale) const {
    static const std::string kOurs = "ForgeModular";

    // What CARTOG measured inside Rack, for every module including ours.
    //
    // Ours used to be read from the manifest their panel was emitted from
    // instead, on the reasoning that it is always present and exact by
    // construction. It was present and it was not exact. MIX declared two
    // params against the six the built module has; SIXMIX declared one
    // against seven, so the six-channel mixer drew a single knob and its
    // channel strip was simply absent. Nobody could see it, because a panel
    // missing controls looks like a panel.
    //
    // That is the same defect as a mixer reporting one input to the
    // generator: a second copy of something, disagreeing with the first. The
    // measurement is taken from the running module and cannot disagree with
    // it, so it wins here as it does everywhere else.
    //
    // The manifest stays as the fallback below, for a machine that has never
    // run a scan -- there, an approximate panel beats an empty one.
    std::vector<KnobSpec> knobs;
    if (const auto* m = PortMap::shared().find(mod.brand, mod.name)) {
        // Rack's units are the layout's units -- a panel is 380 of them tall
        // and one HP is 15 wide -- so these arrive ready to use. The recorded
        // size is what Rack DRAWS, which is why a fader comes out a fader
        // without our knowing anything about the vendor's conventions.
        for (const auto& p : m->params) {
            if (!(p.w > 0.0f)) continue;
            // Only things that ARE knobs, on the same principle the manifest
            // path states above: a slider or a switch drawn as a circle is
            // wrong in a way a reader cannot see. This path drew every control
            // as a knob sized min(w,h), so a fader came out a small dial —
            // the one rule the other path exists to keep.
            //
            // An unclassified control still draws, because every map written
            // before CARTOG classified controls has no kind at all and
            // refusing those would empty panels that draw correctly today.
            if (!PortMap::draws_as_knob(p)) continue;
            knobs.push_back({p.x, p.y, std::min(p.w, p.h), /*already_points=*/true});
        }
        // Everything that is NOT a knob, drawn as what it is.
        //
        // Skipping these was right when the only alternative was drawing a
        // fader as a dial. It is not right as a resting state: MIX's four
        // channel faders and the ten switches across our modules simply were
        // not there, so a four-channel mixer showed its channel labels over
        // bare panel. "Wrong shape" and "no shape" are both wrong; this draws
        // the shape the scan actually recorded.
        for (const auto& p : m->params) {
            if (PortMap::draws_as_knob(p)) continue;
            if (!(p.w > 0.0f) || !(p.h > 0.0f)) continue;
            const float cx = panel.x + p.x * scale;
            const float cy = panel.y + p.y * scale;
            const float w = p.w * scale;
            const float h = p.h * scale;
            if (p.kind == "slider") {
                // A recessed track with a cap across it. The cap sits at the
                // middle, for the same reason a knob's indicator points at a
                // fixed angle: the map records where a control IS, not where
                // it is set, and a cap drawn at an invented position would
                // read as a value.
                const float tw = std::max(1.0f, w * 0.22f);
                canvas.set_fill_color(from_rgb(0x11151B, 1.0f));
                canvas.fill_rounded_rect(cx - tw / 2.0f, cy - h / 2.0f, tw, h,
                                         tw / 2.0f);
                const float ch = std::max(2.0f, h * 0.13f);
                const float cw = std::max(3.0f, w * 0.82f);
                canvas.set_fill_color(from_rgb(0x2A323D, 1.0f));
                canvas.fill_rounded_rect(cx - cw / 2.0f, cy - ch / 2.0f, cw, ch,
                                         std::min(2.0f * scale, ch * 0.4f));
                canvas.set_stroke_color(from_rgb(0x3FD9A4, 1.0f));
                canvas.set_line_width(std::max(0.8f, ch * 0.22f));
                canvas.stroke_line(cx - cw * 0.34f, cy, cx + cw * 0.34f, cy);
            } else {
                // Switches and buttons: a raised nub in a recess. Small enough
                // that anything more detailed is noise at panel scale.
                const float sw = std::max(2.0f, w * 0.72f);
                const float sh = std::max(2.0f, h * 0.72f);
                canvas.set_fill_color(from_rgb(0x11151B, 1.0f));
                canvas.fill_rounded_rect(cx - sw / 2.0f, cy - sh / 2.0f, sw, sh,
                                         std::min(2.0f * scale, sw * 0.3f));
                canvas.set_fill_color(from_rgb(0x3B4553, 1.0f));
                canvas.fill_rounded_rect(cx - sw * 0.32f, cy - sh * 0.32f,
                                         sw * 0.64f, sh * 0.64f,
                                         std::min(1.5f * scale, sw * 0.2f));
            }
        }
    }
    // Only ours have a manifest to fall back to; a vendor module nobody has
    // scanned still draws a plain face rather than a guess at their panel.
    if (knobs.empty() && mod.brand == kOurs) knobs = module_knobs(mod.name);
    if (knobs.empty()) return;

    // A panel is 380 unscaled points tall and PANEL_H_MM millimetres, which is
    // exactly 75 points to the inch. The same constant gives 15 points per HP,
    // so widths agree with the layout without a second conversion.
    const float ppmm = 75.0f / 25.4f * scale;
    for (const auto& k : knobs) {
        const float unit = k.already_points ? scale : ppmm;
        const float cx = panel.x + k.x_mm * unit;
        const float cy = panel.y + k.y_mm * unit;
        const float r = k.diameter_mm * unit / 2.0f;
        if (!(r > 0.5f)) continue;    // below this it is a smudge, not a knob
        // The same shape components.py draws: a rim the knob sinks into, a
        // body, and an indicator running from the centre outward. A dot on the
        // rim vanishes at this size, and a knob whose position cannot be read
        // is the one thing a panel cannot afford.
        canvas.set_fill_color(from_rgb(0x151A21, 1.0f));
        canvas.fill_circle(cx, cy, r);
        canvas.set_fill_color(from_rgb(0x232A35, 1.0f));
        canvas.fill_circle(cx, cy, r * 0.92f);
        canvas.set_stroke_color(from_rgb(0x3FD9A4, 1.0f));
        canvas.set_line_width(std::max(1.0f, r * 0.13f));
        canvas.stroke_line(cx, cy - r * 0.28f, cx, cy - r * 0.80f);
    }
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
    resolve_panel_widths();

    // The SVG cache must hold a WHOLE RACK, or it holds nothing useful.
    //
    // Its default capacity is 16 -- "a handful of frames' worth of distinct
    // docs" -- which is right for a UI that draws a few icons and catastrophic
    // for one that draws a panel per module. A 39-module patch asks for 39
    // distinct documents EVERY FRAME, so a 16-entry LRU evicts all of them and
    // reparses all of them, at up to 120Hz. Measured: 62% CPU on an M5 Max
    // displaying a static patch, with 993 of 1109 samples inside
    // RackPreview::paint sitting in draw_svg -> get_or_build -> the BUILDER,
    // and the cache's own hash table showing up in `erase`.
    //
    // That is the pathological shape of an LRU: a working set one entry larger
    // than capacity does not cost a little more, it costs everything, because
    // every lookup evicts the entry it is about to need. So the cache is sized
    // from the rack rather than left at a default that cannot know about it.
    //
    // Headroom above the module count because a panel is not the only document
    // drawn -- vendor artwork, chrome and the same module at two sizes each
    // take a slot -- and because a cache that is exactly the working set
    // thrashes the moment anything else asks for one.
    // Grown, never shrunk: the cache is global, and a small rack shown after a
    // large one must not evict what the large one needs on the way back.
    static std::size_t s_capacity = 0;
    const std::size_t working_set = modules_.size() * 3 + 64;
    if (working_set > s_capacity) {
        s_capacity = working_set;
        pulp::canvas::SvgDomCache::instance().set_capacity(s_capacity);
    }

    request_repaint();
}

void RackPreview::resolve_panel_widths() {
    // The artwork is the last thing that knows how wide a module is, and it is
    // the only one of the four that is always right there: a .vcv carries no
    // width, a plugin.json carries no width, and a module nobody has run
    // CARTOG over has no entry in the port map. A plugin fetched five minutes
    // ago has none of the three -- which is exactly when somebody is looking
    // at the preview to see what they just got.
    //
    // Every panel it draws is a picture of the real thing at its real size, so
    // its own aspect is the module's width, and the units it was drawn in do
    // not matter.
    static const std::string kOurs = "ForgeModular";
    for (auto& m : modules_) {
        if (m.width_measured) continue;
        const std::string& svg =
            m.brand == kOurs ? panel_svg(m.name) : vendor_svg(m.brand, m.name);
        const int hp = panel_hp_from_artwork(svg);
        if (hp <= 0) continue;   // no artwork, or not a panel: keep the fallback
        m.hp = hp;
        m.width_measured = true;
    }
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
    return layout_rack(modules_, width, height, view_);
}

RackView RackPreview::clamped(RackView v) const {
    const auto b = bounds();
    // Clamped against the rack at the CANDIDATE zoom, not the current one: a
    // zoom out shrinks the rack, and a pan that was legal at the old size has
    // to come back in rather than being left where it no longer reaches.
    RackView at_zoom;
    at_zoom.zoom = v.zoom;
    const auto L = layout_rack(modules_, b.width, b.height, at_zoom);
    const float content_w = L.total_width * L.scale;
    const float content_h = static_cast<float>(L.rows) * kPanelHeight * L.scale;
    return clamp_rack_view(content_w, content_h, b.width, b.height, v);
}

void RackPreview::set_view(RackView v) {
    const auto next = clamped(v);
    if (next == view_) return;
    view_ = next;
    request_repaint();
}

bool RackPreview::zoom_in() {
    const auto before = view_;
    RackView next = view_;
    next.zoom *= kZoomStep;
    set_view(next);
    return view_ != before;
}

bool RackPreview::zoom_out() {
    const auto before = view_;
    RackView next = view_;
    next.zoom /= kZoomStep;
    set_view(next);
    return view_ != before;
}

bool RackPreview::reset_view() {
    const auto before = view_;
    set_view(RackView{});
    return view_ != before;
}

bool RackPreview::pan_by(float dx, float dy) {
    const auto before = view_;
    RackView next = view_;
    next.pan_x += dx;
    next.pan_y += dy;
    set_view(next);
    return view_ != before;
}

bool RackPreview::on_key_event(const pulp::view::KeyEvent& event) {
    if (!event.is_down || !event.isMainModifier()) return false;
    if (event.key == pulp::view::KeyCode::num0) { reset_view(); return true; }
    // KeyCode is ASCII-valued for everything printable -- `semicolon = ';'` is
    // the convention -- and the zoom keys are punctuation the enum does not
    // name, so they are matched on their character.
    //
    // Both faces of each key. "+" is Shift-= and "_" is Shift-- on every
    // layout this ships to, and a binding that answers only the shifted form
    // does nothing for somebody who did not also hold Shift.
    const auto c = static_cast<int>(event.key);
    if (c == '=' || c == '+') { zoom_in(); return true; }
    if (c == '-' || c == '_') { zoom_out(); return true; }
    return false;
}

void RackPreview::on_gesture_event(const pulp::view::GestureEvent& event) {
    // `delta_scale` is the increment since the last event -- a pinch arrives as
    // a stream of small ones -- so it multiplies the zoom rather than setting
    // it. Setting it from `scale`, which is only ever 1 + delta here, would
    // snap back to the fit on every tick of the gesture.
    if (event.delta_scale == 0.0f) {
        pulp::view::View::on_gesture_event(event);
        return;
    }
    RackView next = view_;
    next.zoom *= (1.0f + event.delta_scale);
    set_view(next);
}

void RackPreview::on_mouse_event(const pulp::view::MouseEvent& event) {
    if (!event.is_wheel) {
        pulp::view::View::on_mouse_event(event);
        return;
    }
    // The rack follows the fingers: two fingers down the trackpad reads as a
    // positive delta and walks DOWN the rack, which moves the panels up.
    pan_by(-event.scroll_delta_x, -event.scroll_delta_y);
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
    // The camera, not the fit. Paint and the hit test have to be the same
    // layout or a zoomed rack lights the cable that USED to be under the
    // pointer -- the same class of mistake as flattening a cable two different
    // ways for drawing and for hitting.
    const auto L = layout_rack(modules_, b.width, b.height, view_);
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
    const auto L = layout_rack(modules_, b.width, b.height, view_);

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
        if (artwork) {
            // Knobs, on top of the face -- which is where Rack puts them.
            //
            // A panel SVG is a BACKGROUND. Rack composites every knob, jack
            // and light over it as a separate widget, so a preview that draws
            // only the background shows a module's labels with nothing under
            // them: FREQ, FINE and PW floating over blank plate. Vendor panels
            // happen to have a knob well painted in and so looked right, which
            // is why ours looked uniquely broken beside them.
            //
            // Positions and sizes come from the module's own manifest, the
            // same file the panel was emitted from, so a knob lands exactly
            // where Rack will draw one rather than where we guess.
            draw_screens(canvas, panel, *mod_for_panel, L.scale);
            draw_jacks(canvas, panel, *mod_for_panel, L.scale);
            draw_knobs(canvas, panel, *mod_for_panel, L.scale);
            continue;
        }

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

    // A module drawn without its controls says so, quietly.
    //
    // Quietly on purpose: unlike NOT IN RACK this module is real and will load
    // exactly as drawn -- what is missing is our measurement of its knobs, not
    // the module. Blacking it out would overstate the problem. But saying
    // nothing understates it, because a faceplate with jacks and no controls
    // reads as a module that HAS no controls, and most of the map on this
    // machine is in exactly that state while naming the right plugin version.
    for (const auto& panel : L.panels) {
        if (panel.controls_measured || !panel.available) continue;
        const float h = std::max(9.0f, 11.0f * L.scale);
        canvas.save();
        canvas.clip_rect(panel.x, panel.y, panel.width, panel.height);
        canvas.set_fill_color(pulp::canvas::Color::rgba8(0, 0, 0, 150));
        canvas.fill_rect(panel.x, panel.y + panel.height - h,
                         panel.width, h);
        canvas.set_font(forge::design::type::mono, std::max(7.0f, 8.0f * L.scale));
        canvas.set_fill_color(pulp::canvas::Color::rgba8(0xE8, 0xB3, 0x39));
        canvas.set_text_align(pulp::canvas::TextAlign::center);
        canvas.fill_text("UNMAPPED", panel.x + panel.width / 2.0f,
                         panel.y + panel.height - h * 0.25f);
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

    // A module's own name, for the labels below. The id is what the patch
    // calls it; the name is what its panel says.
    auto name_of = [this](const std::string& id) {
        const auto* m = find(id);
        return m && !m->name.empty() ? m->name : id;
    };
    // A jack's own label, not the id the patch keys it by. "LFO · Square" is
    // what the panel says and what somebody can go and find; "LFO · out0" is
    // an index they would have to count to.
    auto port_label = [this](const std::string& module_id,
                             const std::string& port_id) {
        if (const auto* m = find(module_id)) {
            for (const auto& p : m->ports)
                if (p.id == port_id) return p.name.empty() ? port_id : p.name;
        }
        return port_id;
    };

    // The hovered cable says what it joins, and to what.
    //
    // Dimming the other cables tells you WHICH wire you are on and nothing
    // about where it goes: the ends are jacks among other jacks on panels
    // whose labels are two points tall. The prototype named both ends and
    // outlined the modules, and that is the part that reads.
    if (highlight_ && *highlight_ < connections_.size()) {
        const auto& c = connections_[*highlight_];
        const auto from = port_point(L, modules_, c.from_module, c.from_port,
                                     c.to_module);
        const auto to = port_point(L, modules_, c.to_module, c.to_port,
                                   c.from_module);

        // Outline both panels. A cable has two ends and naming one of them is
        // half an answer.
        for (const auto& id : {c.from_module, c.to_module}) {
            for (const auto& panel : L.panels) {
                if (panel.id != id) continue;
                canvas.set_stroke_color(forge::design::color::accent);
                canvas.set_line_width(std::max(1.5f, 2.0f * L.scale));
                canvas.stroke_rounded_rect(panel.x - 1.0f, panel.y - 1.0f,
                                           panel.width + 2.0f,
                                           panel.height + 2.0f,
                                           4.0f * L.scale);
            }
        }

        // A pill at each end, naming the module and the jack. Drawn last so it
        // sits over the cables rather than under the next one along.
        const std::pair<const JackPoint&, std::pair<std::string, std::string>> tips[] = {
            {from, {name_of(c.from_module),
                    port_label(c.from_module, c.from_port)}},
            {to,   {name_of(c.to_module),
                    port_label(c.to_module, c.to_port)}},
        };
        const float fs = std::max(8.0f, 9.5f * L.scale);
        for (const auto& [p, who] : tips) {
            const std::string text =
                who.second.empty() ? who.first : who.first + " \u00b7 " + who.second;
            canvas.set_font(forge::design::type::mono, fs);
            const float w = canvas.measure_text(text) + 10.0f;
            const float h = fs + 7.0f;
            // Above the jack, nudged inside the view so a label on the top row
            // is not clipped away by the edge it sits against.
            float bx = p.x - w / 2.0f;
            float by = p.y - h - 8.0f * L.scale;
            bx = std::max(2.0f, std::min(bx, bounds().width - w - 2.0f));
            if (by < 2.0f) by = p.y + 8.0f * L.scale;
            canvas.set_fill_color(pulp::canvas::Color::rgba8(16, 18, 26, 240));
            canvas.fill_rounded_rect(bx, by, w, h, 3.0f);
            canvas.set_stroke_color(forge::design::color::accent);
            canvas.set_line_width(1.0f);
            canvas.stroke_rounded_rect(bx, by, w, h, 3.0f);
            canvas.set_fill_color(pulp::canvas::Color::rgba8(0xF2, 0xF5, 0xFA));
            canvas.set_text_align(pulp::canvas::TextAlign::center);
            canvas.fill_text(text, bx + w / 2.0f, by + h - 5.0f);
            canvas.set_text_align(pulp::canvas::TextAlign::left);
        }
    }
}

}  // namespace forge_modular
