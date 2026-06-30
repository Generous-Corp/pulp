// create_view() — the DDFX editor as a native Pulp UI, triaz-demo style.
// The visual is DDFX's real captured editor SVG (MainEditor.svg via --ui-export),
// rendered pixel-faithfully by DesignFrameView. Interactive controls are
// declared as DesignFrameElements pointing at named SVG sub-elements and wired
// ONE AT A TIME (the macro knobs first). No custom Canvas paint, no JUCE embed.
#include "processor.hpp"
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/state/store.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <memory>
#include <sstream>
#include <map>
#include <string>
#include <vector>

namespace knobpg {

namespace vw = pulp::view;

// Semantic colours (DDFX brand). CONTROL_ACCENT = the purple from
// Projects/DreamDateFX/Resources/Metadata.json "controlAccent" (JUCE:
// theme_.getControlAccent()) — NOT the yssUI default darkBlue #697896. In the
// captured SVG the macro-knob needles render with exactly this fill, which is
// how we'll locate them when wiring controls.
inline constexpr const char* CONTROL_ACCENT = "#7b6896";  // DDFX accent (purple)
inline constexpr const char* CONTROL_LABEL  = "#a19b92";  // macro-label / icon brown (kLabel)

// Hover affordance: how far a control's colour shifts toward white on hover, as
// a fraction of its headroom to white (0..1). ONE knob for every control's
// hover state. Lighten-toward-white (not a brightness multiply) keeps already
// pale colours like the cream macro ring distinct from the panel instead of
// blowing them out to a flat wash.
inline constexpr float HOVER_LIGHTEN = 0.20f;

// Bypass: how far a bypassed module's whole column (slot panel + dot + knob +
// labels) fades toward the panel background — drawn as a translucent panel-
// coloured rect over the column, so everything dims by the SAME amount (the
// JUCE behaviour). 0 = no fade, 1 = fully background.
inline constexpr float BYPASS_DIM = 0.6f;

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// JUCE exports a stroked ring (DDDTheme drawEllipse) as an ANNULUS — an outer
// circle subpath + a CONCENTRIC inner circle subpath — with NO fill-rule. Pulp's
// draw_svg defaults to nonzero winding, so it fills the whole thing solid (a
// disc). Add fill-rule="evenodd" so the inner subpath cuts a hole → a stroke.
// Discriminator: two subpaths whose first move-to share the same x (concentric
// circles). This catches macro rings (#e8e1d5), the Threshold ring (#a19b92),
// and radio rings — colour-agnostic — without touching text glyphs (their hole
// subpath isn't at the same x) or single-subpath filled discs.
static void fix_stroked_rings(std::string& svg) {
    static const std::string kRule = " fill-rule=\"evenodd\"";
    size_t p = 0;
    while ((p = svg.find("<path ", p)) != std::string::npos) {
        const size_t end = svg.find("/>", p);
        if (end == std::string::npos) break;
        const std::string el = svg.substr(p, end - p);
        bool concentric_ring = false;
        const size_t d = el.find("d=\"M");
        if (d != std::string::npos && el.find('C') != std::string::npos) {
            const float x1 = std::strtof(el.c_str() + d + 4, nullptr);
            const size_t z = el.find('Z', d);
            const size_t m2 = (z != std::string::npos) ? el.find('M', z) : std::string::npos;
            if (m2 != std::string::npos) {
                const float x2 = std::strtof(el.c_str() + m2 + 1, nullptr);
                concentric_ring = std::abs(x1 - x2) < 1.5f;
            }
        }
        if (concentric_ring && el.find("fill-rule") == std::string::npos) {
            svg.insert(end, kRule);
            p = end + kRule.size() + 2;
        } else {
            p = end + 2;
        }
    }
}

// Parse the first "M<x>,<y>" of a path's d attribute.
static bool first_move(const std::string& el, float& x, float& y) {
    const auto d = el.find("d=\"M");
    if (d == std::string::npos) return false;
    const char* s = el.c_str() + d + 4;
    char* e = nullptr;
    x = std::strtof(s, &e);
    if (e == s || *e != ',') return false;
    y = std::strtof(e + 1, nullptr);
    return true;
}

// Centroid of a triangle path ("M x,y L x,y L x,y Z"): mean of all "n,n" coords.
static bool triangle_centroid(const std::string& el, float& cx, float& cy) {
    const auto d = el.find("d=\""); if (d == std::string::npos) return false;
    const char* s = el.c_str() + d + 3;
    const char* end = std::strchr(s, '"'); if (!end) return false;
    float sx = 0, sy = 0; int n = 0;
    for (const char* p = s; p < end; ) {
        if ((*p == 'M' || *p == 'L') ) { ++p; continue; }
        if (*p == ' ' || *p == 'Z' || *p == 'z') { ++p; continue; }
        char* e = nullptr; float x = std::strtof(p, &e);
        if (e == p || *e != ',') { ++p; continue; }
        float y = std::strtof(e + 1, &e);
        sx += x; sy += y; ++n; p = e;
    }
    if (n == 0) return false;
    cx = sx / n; cy = sy / n;
    return true;
}

// Find the nearest UNTAGGED single-subpath triangle of `fill` to (cx,cy) within
// the y-band, tag it `id`, and wrap it in a static rotate so DesignFrameView's
// (value-0.5)*270 needle rotation puts value 0 at 7 o'clock (the rotary start).
// Returns the needle marker (id="...") or "" if none found. Shared by every knob.
static std::string tag_wrap_needle(std::string& svg, float cx, float cy,
                                   const char* fill, const std::string& id,
                                   float yblo, float ybhi, float max_dist) {
    const std::string fillattr = std::string("fill=\"") + fill + "\"";
    size_t best = std::string::npos, best_end = 0;
    float bestd = 1e9f;
    for (size_t p = svg.find("<path "); p != std::string::npos; p = svg.find("<path ", p + 1)) {
        const size_t end = svg.find("/>", p);
        if (end == std::string::npos) break;
        const std::string el = svg.substr(p, end - p);
        if (el.find(fillattr) == std::string::npos) continue;
        if (el.find(" id=\"") != std::string::npos) continue;  // already tagged
        float x, y;
        if (!first_move(el, x, y) || y < yblo || y > ybhi) continue;
        const size_t z = el.find('Z');                          // single-subpath only
        if (z != std::string::npos && el.find('M', z) != std::string::npos) continue;
        const float dd = std::abs(x - cx) + std::abs(y - cy);
        if (dd < bestd) { bestd = dd; best = p; best_end = end; }
    }
    if (best == std::string::npos || bestd > max_dist) return "";
    const std::string tri = svg.substr(best, best_end - best);
    float gx = 0, gy = 0; triangle_centroid(tri, gx, gy);
    float theta = std::atan2(gx - cx, -(gy - cy)) * 180.0f / 3.14159265f;
    if (theta < 0) theta += 360.0f;
    const float stat = 360.0f - theta;
    svg.insert(best_end, " id=\"" + id + "\"");
    char wrap_open[96];
    std::snprintf(wrap_open, sizeof(wrap_open), "<g transform=\"rotate(%.3f %.3f %.3f)\">", stat, cx, cy);
    const size_t path_end = svg.find("/>", best) + 2;
    svg.insert(path_end, "</g>");
    svg.insert(best, wrap_open);
    return "id=\"" + id + "\"";
}

struct MacroKnobInfo { float cx, cy; std::string needle_id; const char* param; };

// Wire the 8 macro knobs (component #1). The visual is already the SVG; here we
// (a) tag each macro triangle (#7b6896, the 5xx y-band) with an id, (b) wrap it
// in a static rotate(135, cx, cy) so DesignFrameView's (value-0.5)*270 needle
// rotation lands DDFX's value0=rest / value1=+270deg sweep, and (c) return a
// knob element per triangle keyed to its ring centre + APVTS param.
static std::vector<MacroKnobInfo> wire_macro_knobs(std::string& svg) {
    static const char* PARAMS[8] = {"lfo_macro", "fx1_macro", "fx2_macro", "fx3_macro",
                                    "fx4_macro", "fx5_macro", "fx6_macro", "master_output"};
    constexpr float CY = 531.7f;          // macro row centre (all 8 share it)

    // 1. Ring centres: #e8e1d5 circles whose first move is the macro-ring top band.
    std::vector<float> ring_x;
    for (size_t p = svg.find("<path "); p != std::string::npos; p = svg.find("<path ", p + 1)) {
        const size_t end = svg.find("/>", p);
        if (end == std::string::npos) break;
        const std::string el = svg.substr(p, end - p);
        if (el.find("fill=\"#e8e1d5\"") == std::string::npos) continue;
        float x, y;
        if (first_move(el, x, y) && y > 475.0f && y < 490.0f) ring_x.push_back(x);
    }
    std::sort(ring_x.begin(), ring_x.end());

    // 2. Pair each ring with its nearest macro triangle (#7b6896, knob y-band).
    std::vector<MacroKnobInfo> out;
    for (size_t i = 0; i < ring_x.size() && i < 8; ++i) {
        const std::string needle = tag_wrap_needle(
            svg, ring_x[i], CY, "#7b6896", "mk" + std::to_string(i), 510.0f, 575.0f, 60.0f);
        if (!needle.empty()) out.push_back({ring_x[i], CY, needle, PARAMS[i]});
    }
    return out;
}

// The Threshold/SaturnRingKnob (MASTER strip): its ring is the concentric
// #a19b92 circle of radius ~27 at (1141,359). Wire it like a macro knob.
static bool wire_threshold_knob(std::string& svg, MacroKnobInfo& out) {
    // Find the threshold ring centre: a concentric #a19b92 circle, radius 20..40.
    float cx = 0, cy = 0; bool found = false;
    for (size_t p = svg.find("<path "); p != std::string::npos && !found; p = svg.find("<path ", p + 1)) {
        const size_t end = svg.find("/>", p);
        if (end == std::string::npos) break;
        const std::string el = svg.substr(p, end - p);
        if (el.find("fill=\"#a19b92\"") == std::string::npos || el.find('C') == std::string::npos) continue;
        const size_t d = el.find("d=\"M");
        if (d == std::string::npos) continue;
        char* e = nullptr; const float x1 = std::strtof(el.c_str() + d + 4, &e);
        if (*e != ',') continue; const float y1 = std::strtof(e + 1, nullptr);
        const size_t z = el.find('Z', d), m2 = (z != std::string::npos) ? el.find('M', z) : std::string::npos;
        if (m2 == std::string::npos) continue;
        if (std::abs(std::strtof(el.c_str() + m2 + 1, nullptr) - x1) > 1.5f) continue;  // concentric
        // radius from the d's x-extent (start INSIDE d=" so we don't stop at it)
        float minx = 1e9f, maxx = -1e9f;
        for (const char* q = el.c_str() + d + 3; *q && *q != '"'; ) {
            if ((*q >= '0' && *q <= '9')) { float v = std::strtof(q, (char**)&q); if (*q == ',') { minx = std::min(minx, v); maxx = std::max(maxx, v); } }
            else ++q;
        }
        const float r = (maxx - minx) / 2.0f;
        if (r > 20.0f && r < 40.0f) { cx = x1; cy = y1 + r; found = true; }
    }
    if (!found) return false;
    const std::string needle = tag_wrap_needle(svg, cx, cy, "#a19b92", "thr",
                                               cy - 40.0f, cy + 40.0f, 40.0f);
    if (needle.empty()) return false;
    out = {cx, cy, needle, "limiter_threshold"};
    return true;
}

namespace cv = pulp::canvas;

// ── Radio (Sine/Square/Saw): faithful click-to-select ────────────────────────
// 3 #a19b92 circles in the LFO module; the selected one is FILLED, the others
// stroked rings. We un-bake the baked fill (so the SVG shows all three as rings)
// and the DdfxEditorView draws the filled dot on the LIVE selection + routes
// clicks. No stock DesignFrameView kind does exclusive circle-fill selection.
struct RadioInfo { float cx = 0, r = 8; std::vector<float> cys; int sel = 0; bool ok = false; };

static float circle_radius(const std::string& el) {
    const size_t d = el.find("d=\"M"); if (d == std::string::npos) return 0.0f;
    float minx = 1e9f, maxx = -1e9f;
    for (const char* q = el.c_str() + d + 3; *q && *q != '"'; ) {
        if (*q >= '0' && *q <= '9') { float v = std::strtof(q, (char**)&q); if (*q == ',') { minx = std::min(minx, v); maxx = std::max(maxx, v); } }
        else ++q;
    }
    return (maxx > minx) ? (maxx - minx) / 2.0f : 0.0f;
}

static RadioInfo wire_radio(std::string& svg) {
    RadioInfo ri;
    struct C { float cx, cy, r; bool filled; size_t pos, end; };
    std::vector<C> cs;
    for (size_t p = svg.find("<path "); p != std::string::npos; p = svg.find("<path ", p + 1)) {
        const size_t end = svg.find("/>", p); if (end == std::string::npos) break;
        const std::string el = svg.substr(p, end - p);
        if (el.find("fill=\"#a19b92\"") == std::string::npos || el.find('C') == std::string::npos) continue;
        float x, y; if (!first_move(el, x, y)) continue;
        if (x < 115.0f || x > 150.0f || y < 210.0f || y > 300.0f) continue;     // LFO radio box
        const float r = circle_radius(el);
        const size_t z = el.find('Z');
        const bool annular = z != std::string::npos && el.find('M', z) != std::string::npos;
        cs.push_back({x, y + r, r, !annular, p, end});
    }
    if (cs.size() < 3) return ri;
    std::sort(cs.begin(), cs.end(), [](const C& a, const C& b) { return a.cy < b.cy; });
    ri.cx = cs[0].cx; ri.r = cs[0].r;
    for (const auto& c : cs) ri.cys.push_back(c.cy);
    for (size_t i = 0; i < cs.size(); ++i) if (cs[i].filled) ri.sel = (int)i;
    // un-bake the filled (selected) circle → a stroked ring (back→front so the
    // earlier paths' positions stay valid through the length-changing replace).
    for (auto it = cs.rbegin(); it != cs.rend(); ++it) {
        if (!it->filled) continue;
        const std::string el = svg.substr(it->pos, it->end - it->pos);
        const size_t f = el.find("fill=\"#a19b92\"");
        if (f != std::string::npos)
            svg.replace(it->pos + f, std::string("fill=\"#a19b92\"").size(),
                        "fill=\"none\" stroke=\"#a19b92\" stroke-width=\"2.5\"");
    }
    ri.ok = true;
    return ri;
}

// Read the SVG's intrinsic width/height (viewBox falls back to width=/height=).
static void svg_intrinsic(const std::string& svg, float& w, float& h) {
    w = h = 0.0f;
    const size_t vb = svg.find("viewBox=\"");
    if (vb != std::string::npos) {
        float x0, y0;
        if (std::sscanf(svg.c_str() + vb + 9, "%f %f %f %f", &x0, &y0, &w, &h) == 4) return;
    }
    const size_t wp = svg.find(" width=\""), hp = svg.find(" height=\"");
    if (wp != std::string::npos) w = std::strtof(svg.c_str() + wp + 8, nullptr);
    if (hp != std::string::npos) h = std::strtof(svg.c_str() + hp + 9, nullptr);
}

// One hover target for a knob: where to hit-test, the ring colour to brighten,
// and the needle (so it can be re-drawn brightened on top — the Master is a
// SOLID disc whose brightened-ring overlay would otherwise hide its triangle).
// bbox is the half-extent searched in the SVG for that knob's ring/disc.
struct KnobHover {
    float cx = 0, cy = 0, hit = 0, bbox = 0;
    std::string ring;        // ring/disc fill colour (e.g. #e8e1d5)
    std::string needle_id;   // id="..." of the needle path (see tag_wrap_needle)
};

// A rectangular click + hover target: a top-bar icon button (preset ‹ ›, dice,
// gear) or a macro bypass dot. Icon buttons are momentary (click = action);
// bypass dots toggle, dimming when off. cx/cy + hw/hh is the hover/click box.
struct Rect4 { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

struct HitTarget {
    float cx = 0, cy = 0, hw = 0, hh = 0;
    std::string id;
    bool toggle = false;     // bypass dot toggles; icon buttons don't
    bool on = true;          // toggle state (dot active)
    int knob_index = -1;     // for a bypass dot: the macro knob it disables
    // When toggled OFF, fade these rects toward the panel. A bypass module needs
    // two (the slot panel above + the dot/knob/label below) because the slot row
    // (140px pitch) and knob row (130px) are offset in the captured SVG. Their Y
    // ranges are disjoint, so two rects never double-darken the overlap.
    std::vector<Rect4> dim;
};

// The panel background — a dimmed bypass dot blends toward this.
inline constexpr const char* PANEL_BG = "#fbf4e6";

// Lighten #rrggbb toward white by `amount` (0..1): c += amount·(255−c). Shifting a
// fraction of each channel's remaining headroom asymptotes to white and never
// clips a near-white colour into a flat wash (the cream macro ring stays distinct
// from the panel). Pass a partial amount to EASE the shift (e.g. animated hover).
static std::string lighten_hex(const std::string& hex, float amount) {
    if (hex.size() < 7 || hex[0] != '#') return hex;
    auto byte = [&](int i) { return (int)std::strtol(hex.substr((size_t)i, 2).c_str(), nullptr, 16); };
    auto lighten = [&](int c) {
        const int v = (int)((float)c + amount * (255.0f - (float)c) + 0.5f);
        return v > 255 ? 255 : (v < 0 ? 0 : v);
    };
    char out[8];
    std::snprintf(out, sizeof(out), "#%02x%02x%02x", lighten(byte(1)), lighten(byte(3)), lighten(byte(5)));
    return out;
}

// The full hover shift — HOVER_LIGHTEN drives every control's hover. One knob.
static std::string brighten_hex(const std::string& hex) { return lighten_hex(hex, HOVER_LIGHTEN); }

// Remove a colour's hue → its luminance grey (Rec.601). A bypassed (disabled)
// module's accent — its triangle indicator and the dot's hover — go neutral grey.
static std::string desaturate_hex(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return hex;
    auto byte = [&](int i) { return (int)std::strtol(hex.substr((size_t)i, 2).c_str(), nullptr, 16); };
    int y = (int)(0.299f * (float)byte(1) + 0.587f * (float)byte(3) + 0.114f * (float)byte(5) + 0.5f);
    if (y > 255) y = 255;
    char out[8];
    std::snprintf(out, sizeof(out), "#%02x%02x%02x", y, y, y);
    return out;
}

// #rrggbb → canvas Color.
static cv::Color hex_color(const std::string& hex) {
    auto by = [&](int i) { return (uint8_t)std::strtol(hex.substr((size_t)i, 2).c_str(), nullptr, 16); };
    return cv::Color::rgba8(by(1), by(3), by(5));
}

// A disk preset: name (file stem) and tag (its subfolder), like the JUCE browser
// which derives the tag from the preset's folder. path kept for later loading.
struct Preset { std::string name, tag, path; };

// Recursively scan `dir` for *.dddp, building the preset list (name = file stem,
// tag = immediate parent folder). Sorted by name to match the dropdown order.
static std::vector<Preset> scan_presets(const std::string& dir) {
    std::vector<Preset> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const auto& p = it->path();
        if (p.extension() != ".dddp") continue;
        out.push_back({p.stem().string(), p.parent_path().filename().string(), p.string()});
    }
    std::sort(out.begin(), out.end(), [](const Preset& a, const Preset& b) { return a.name < b.name; });
    return out;
}

// All immediate subfolders of `dir` (the tag set), sorted — including empty ones,
// matching the JUCE browser which lists empty tag folders too.
static std::vector<std::string> scan_tags(const std::string& dir) {
    std::set<std::string> s;
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec))
        for (auto it = std::filesystem::directory_iterator(dir, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
            if (it->is_directory(ec)) s.insert(it->path().filename().string());
    return {s.begin(), s.end()};
}

// Remove every `fill`-coloured <path> whose first point is inside the box (used
// to drop the baked "No Preset" header glyphs so we can draw a live name there).
static void strip_region(std::string& svg, const std::string& fill,
                         float x0, float y0, float x1, float y1) {
    const std::string fa = "fill=\"" + fill + "\"";
    size_t p = 0;
    while ((p = svg.find("<path ", p)) != std::string::npos) {
        const size_t end = svg.find("/>", p);
        if (end == std::string::npos) break;
        const std::string el = svg.substr(p, end - p + 2);
        float x, y;
        if (el.find(fa) != std::string::npos && first_move(el, x, y) &&
            x >= x0 && x <= x1 && y >= y0 && y <= y1) {
            svg.erase(p, end - p + 2);   // removed → don't advance p
        } else {
            p = end + 2;
        }
    }
}

// One effect type in the picker grid: display label + its icon SVG.
struct FxType { std::string label, icon; };

// One control in an effect's "Controls" view — a 1:1 port of a JUCE
// EffectParameter (same row/col/type/label/options). The grid is the SAME 6×2
// the icon picker uses, so each control lines up under its icon column.
struct FxControl {
    int row, col;                                  // 1-based grid position
    enum Kind { Knob, Toggle, Radio, Meter } kind; // Meter = compressor GR meter (read-only)
    const char* label;                             // param label (cell bottom)
    float def;                                      // knob default 0..1, or selected option index
    std::vector<std::string> options;              // Toggle / Radio option labels
    float modDep = 0.0f;                            // Saturn-ring default mod depth (.modulatable arg)
    int rowSpan = 1;                                // radio spanning 2 rows (.span) → centred over both
    int colSpan = 1;                                // meter spanning 2 cols (.span)
    bool assignable() const { return kind != Meter; }   // Meter can't be a slot control
};

// DELAY — exact layout from yssAudio DelayDefinition.h (createDelayDefinition):
//   Row 1: Unit(Free/Sync) · Time · Feedback · Mod · Reverse(Off/On) · Quality(Lo-Fi/Modern)
//   Row 2: Drive · Offset · Low Cut · High Cut · Mix · Mode(Mono/Stereo/Ping-Pong)
// Knobs are Saturn-ring (modulatable); Unit/Reverse/Quality are 2-state toggles;
// Mode is a vertical radio group. Defaults are the .range(...) third arg.
inline const std::vector<FxControl>& delay_controls() {
    static const std::vector<FxControl> c = {
        {1, 1, FxControl::Toggle, "Unit",     0.0f,    {"Free", "Sync"}},
        {1, 2, FxControl::Knob,   "Time",     0.4286f, {}},
        {1, 3, FxControl::Knob,   "Feedback", 0.5f,    {}},
        {1, 4, FxControl::Knob,   "Mod",      0.1f,    {}},
        {1, 5, FxControl::Toggle, "Reverse",  0.0f,    {"Off", "On"}},
        {1, 6, FxControl::Toggle, "Quality",  0.0f,    {"Lo-Fi", "Modern"}},
        {2, 1, FxControl::Knob,   "Drive",    0.33f,   {}},
        {2, 2, FxControl::Knob,   "Offset",   0.5f,    {}},
        {2, 3, FxControl::Knob,   "Low Cut",  0.4f,    {}},
        {2, 4, FxControl::Knob,   "High Cut", 0.7f,    {}},
        {2, 5, FxControl::Knob,   "Mix",      0.0f,    {}, 1.0f},   // .modulatable(1.0)
        {2, 6, FxControl::Radio,  "Mode",     0.0f,    {"Mono", "Stereo", "Ping-Pong"}},
    };
    return c;
}

// BIT CRUSHER — exact layout from BitCrusherDefinition.h (columns 1 & 6 empty):
//   Row 1: Bits · Rate · Filter · Mode(Modern/Vintage)
//   Row 2: Crush · Jitter · Flt Route(Post/Pre) · Mix
// (The Vintage-mode twins Resample/Alias share grid cells with Rate/Jitter and are
// omitted here, as timeSync was for Delay — default mode Modern shows Rate/Jitter.)
inline const std::vector<FxControl>& bit_crusher_controls() {
    static const std::vector<FxControl> c = {
        {1, 2, FxControl::Knob,   "Bits",      0.714f, {}},
        {1, 3, FxControl::Knob,   "Rate",      0.856f, {}},
        {1, 4, FxControl::Knob,   "Filter",    1.0f,   {}},
        {1, 5, FxControl::Toggle, "Mode",      0.0f,   {"Modern", "Vintage"}},
        {2, 2, FxControl::Knob,   "Crush",     0.0f,   {}},
        {2, 3, FxControl::Knob,   "Jitter",    0.0f,   {}},
        {2, 4, FxControl::Toggle, "Flt Route", 0.0f,   {"Post", "Pre"}},
        {2, 5, FxControl::Knob,   "Mix",       0.0f,   {}, 1.0f},   // .modulatable(1.0)
    };
    return c;
}

// The remaining effect tables — each ported 1:1 from its <Fx>Definition.h.
// (Special control types — Compressor GR meter, Space IR picker — are omitted
// for now, as are mode-swap twins like Modulation/Width rateSync.)
inline const std::vector<FxControl>& compressor_controls() {
    static const std::vector<FxControl> c = {
        {1, 2, FxControl::Meter, "GR",      0.0f,  {}, 0.0f, 1, 2},   // GR meter, spans cols 2-3
        {1, 4, FxControl::Knob, "Attack",  0.5f,  {}},
        {1, 5, FxControl::Knob, "Makeup",  0.0f,  {}},
        {2, 2, FxControl::Knob, "Thresh",  1.0f,  {}},
        {2, 3, FxControl::Knob, "Ratio",   0.46f, {}},
        {2, 4, FxControl::Knob, "Release", 0.59f, {}},
        {2, 5, FxControl::Knob, "Mix",     0.0f,  {}, 1.0f},
    };
    return c;
}
inline const std::vector<FxControl>& eq_controls() {
    static const std::vector<FxControl> c = {
        {1, 2, FxControl::Knob, "LF Freq",  0.512f, {}},
        {1, 3, FxControl::Knob, "LMF Freq", 0.509f, {}},
        {1, 4, FxControl::Knob, "HMF Freq", 0.490f, {}},
        {1, 5, FxControl::Knob, "HF Freq",  0.509f, {}},
        {2, 2, FxControl::Knob, "LF Gain",  0.5f,   {}},
        {2, 3, FxControl::Knob, "LMF Gain", 0.5f,   {}},
        {2, 4, FxControl::Knob, "HMF Gain", 0.5f,   {}},
        {2, 5, FxControl::Knob, "HF Gain",  0.5f,   {}},
        {2, 6, FxControl::Knob, "Scale",    0.0f,   {}, 1.0f},
    };
    return c;
}
inline const std::vector<FxControl>& filter_controls() {
    static const std::vector<FxControl> c = {
        {1, 2, FxControl::Knob,  "Cutoff", 0.0f, {}, 1.0f},
        {1, 3, FxControl::Knob,  "Res",    0.0f, {}},
        {1, 4, FxControl::Knob,  "Drive",  0.0f, {}},
        {2, 3, FxControl::Radio, "Type",   0.0f, {"LPF", "HPF", "BPF"}},
        {2, 4, FxControl::Toggle, "Slope", 0.0f, {"2-Pole", "4-Pole"}},
        {2, 5, FxControl::Toggle, "Style", 0.0f, {"Ladder", "SVF"}},
    };
    return c;
}
inline const std::vector<FxControl>& distortion_controls() {
    static const std::vector<FxControl> c = {
        {1, 4, FxControl::Knob,  "Drive", 0.0f, {}, 1.0f},
        {1, 5, FxControl::Knob,  "Tone",  0.5f, {}},
        {2, 2, FxControl::Radio, "Mode",  0.0f, {"Subtle", "Smooth", "Tape", "Valve", "Crunch", "Clip"}, 0.0f, 2},
        {2, 4, FxControl::Knob,  "Trim",  0.5f, {}},
        {2, 5, FxControl::Knob,  "Mix",   1.0f, {}},
    };
    return c;
}
inline const std::vector<FxControl>& ring_mod_controls() {
    static const std::vector<FxControl> c = {
        {1, 2, FxControl::Radio, "Wave",     0.0f,  {"Sine", "Tri", "Square"}},
        {1, 3, FxControl::Knob,  "Freq",     0.25f, {}},
        {1, 4, FxControl::Knob,  "Spread",   0.0f,  {}},
        {1, 5, FxControl::Knob,  "Depth",    1.0f,  {}},
        {2, 2, FxControl::Knob,  "Low Cut",  0.0f,  {}},
        {2, 3, FxControl::Knob,  "High Cut", 1.0f,  {}},
        {2, 4, FxControl::Knob,  "Feedback", 0.0f,  {}},
        {2, 5, FxControl::Knob,  "Mix",      0.0f,  {}, 1.0f},
    };
    return c;
}
inline const std::vector<FxControl>& modulation_controls() {   // rateSync omitted
    static const std::vector<FxControl> c = {
        {1, 2, FxControl::Knob,   "Rate",     0.2f, {}},
        {1, 3, FxControl::Knob,   "Depth",    0.5f, {}},
        {1, 4, FxControl::Knob,   "Low Cut",  0.3f, {}},
        {1, 5, FxControl::Knob,   "Stereo",   0.5f, {}},
        {2, 1, FxControl::Radio,  "Mode",     0.0f, {"Phaser", "Chorus", "Flanger"}},
        {2, 2, FxControl::Toggle, "Sync",     0.0f, {"Hz", "Sync"}},
        {2, 3, FxControl::Knob,   "Feedback", 0.5f, {}},
        {2, 4, FxControl::Knob,   "High Cut", 0.7f, {}},
        {2, 5, FxControl::Knob,   "Mix",      0.0f, {}, 1.0f},
    };
    return c;
}
inline const std::vector<FxControl>& width_controls() {   // "Motion"; rateSync omitted
    static const std::vector<FxControl> c = {
        {1, 3, FxControl::Toggle, "Random",       0.0f, {"Off", "On"}},
        {1, 4, FxControl::Knob,   "Stereo Phase", 0.5f, {}},
        {2, 2, FxControl::Radio,  "Wave",         0.0f, {"Sine", "Triangle", "Square"}},
        {2, 3, FxControl::Knob,   "Rate",         0.3f, {}},
        {2, 4, FxControl::Knob,   "Depth",        0.0f, {}, 1.0f},
        {2, 5, FxControl::Toggle, "Sync",         0.0f, {"Hz", "Sync"}},
    };
    return c;
}
inline const std::vector<FxControl>& lofi_controls() {
    static const std::vector<FxControl> c = {
        {1, 2, FxControl::Toggle, "Wobble",   0.0f, {"Sine", "Random"}},
        {1, 3, FxControl::Knob,   "Low Res",  0.5f, {}},
        {1, 4, FxControl::Knob,   "High Res", 0.5f, {}},
        {1, 5, FxControl::Radio,  "Quality",  0.0f, {"High", "Med", "Low"}},
        {2, 1, FxControl::Knob,   "Rate",     0.5f, {}},
        {2, 2, FxControl::Knob,   "Wow",      0.0f, {}, 1.0f},
        {2, 3, FxControl::Knob,   "Low Cut",  0.5f, {}},
        {2, 4, FxControl::Knob,   "High Cut", 0.5f, {}, 0.25f},
        {2, 5, FxControl::Knob,   "Flutter",  0.0f, {}, 1.0f},
        {2, 6, FxControl::Knob,   "Mix",      1.0f, {}},
    };
    return c;
}
inline const std::vector<FxControl>& reverb_controls() {   // FDNPresetVerb
    static const std::vector<FxControl> c = {
        {1, 1, FxControl::Radio, "Mode",     0.0f,  {"Room", "Hall", "Galaxy"}},
        {2, 1, FxControl::Knob,  "PreDelay", 0.0f,  {}},
        {1, 2, FxControl::Knob,  "Size",     1.0f,  {}},
        {2, 2, FxControl::Knob,  "Decay",    0.85f, {}},
        {1, 3, FxControl::Knob,  "Drive",    0.5f,  {}},
        {2, 3, FxControl::Knob,  "Shimmer",  0.0f,  {}},
        {1, 4, FxControl::Knob,  "Damping",  0.8f,  {}},
        {2, 4, FxControl::Knob,  "Bass Cut", 0.0f,  {}},
        {1, 5, FxControl::Knob,  "Mod",      0.0f,  {}},
        {2, 5, FxControl::Knob,  "Attack",   0.0f,  {}},
        {1, 6, FxControl::Knob,  "Width",    0.5f,  {}},
        {2, 6, FxControl::Knob,  "Mix",      0.0f,  {}, 1.0f},
    };
    return c;
}
inline const std::vector<FxControl>& space_controls() {   // IR picker (row 1) omitted
    static const std::vector<FxControl> c = {
        {2, 2, FxControl::Knob, "Pre-Delay", 0.0f, {}},
        {2, 3, FxControl::Knob, "Low Cut",   0.0f, {}},
        {2, 4, FxControl::Knob, "High Cut",  1.0f, {}},
        {2, 5, FxControl::Knob, "Mix",       0.0f, {}, 1.0f},
    };
    return c;
}

// LFO (audio-effect variant — createLFOEffectDefinition, layout 2×6): Wave radio
// (row 2 col 1, single-column), Sync / Host Sync toggles (row 1, cols 2/3), Rate
// knob + Random toggle (row 2, cols 2/3). Row 1 col 1 holds the LFO visualiser
// and cols 4–6 hold the 6 depth sliders — both drawn separately for the LFO slot.
inline const std::vector<FxControl>& lfo_controls() {
    static const std::vector<FxControl> c = {
        {2, 1, FxControl::Radio,  "Wave",      0.0f, {"Sine", "Square", "Saw"}, 0.0f},
        {1, 2, FxControl::Toggle, "Sync",      0.0f, {"Hz", "Sync"}},
        {2, 2, FxControl::Knob,   "Rate",      0.3f, {}, 0.0f},
        {1, 3, FxControl::Toggle, "Host Sync", 0.0f, {"Off", "On"}},
        {2, 3, FxControl::Toggle, "Random",    0.0f, {"Off", "On"}},
    };
    return c;
}

// The control table for a given effect name (empty = not ported / no controls).
inline const std::vector<FxControl>& effect_controls(const std::string& fx) {
    if (fx == "LFO")         return lfo_controls();
    if (fx == "Delay")       return delay_controls();
    if (fx == "Bit Crusher") return bit_crusher_controls();
    if (fx == "Compressor")  return compressor_controls();
    if (fx == "EQ")          return eq_controls();
    if (fx == "Filter")      return filter_controls();
    if (fx == "Distortion")  return distortion_controls();
    if (fx == "Ring Mod")    return ring_mod_controls();
    if (fx == "Modulation")  return modulation_controls();
    if (fx == "Width")       return width_controls();
    if (fx == "Lo-Fi")       return lofi_controls();
    if (fx == "Reverb")      return reverb_controls();
    if (fx == "Space")       return space_controls();
    static const std::vector<FxControl> none;
    return none;
}

class DdfxEditorView : public vw::DesignFrameView {
public:
    DdfxEditorView(std::string svg, std::vector<vw::DesignFrameElement> els,
                   RadioInfo radio, std::vector<KnobHover> knobs, std::vector<HitTarget> hits,
                   std::vector<Preset> presets, std::vector<std::string> tags,
                   std::vector<FxType> fx_types)
        : vw::DesignFrameView(svg, std::move(els)),
          svg_copy_(std::move(svg)), radio_(std::move(radio)),
          knobs_(std::move(knobs)), hits_(std::move(hits)), presets_(std::move(presets)),
          tags_(std::move(tags)), fx_types_(std::move(fx_types)), rng_(std::random_device{}()) {
        svg_intrinsic(svg_copy_, svg_w_, svg_h_);
        set_focusable(true);   // so a click claims keyboard focus (search typing)
        // Right-click a slot control → open the param-picker (customize the layout).
        on_context_menu = [this](vw::Point p) {
            const auto t = panel_transform(local_bounds());
            if (t.scale <= 0.0f || fx_menu_module_ >= 0) return;
            const float px = (p.x - t.ox) / t.scale, py = (p.y - t.oy) / t.scale;
            int m, pos, ring;
            if (slot_ctl_at(px, py, m, pos, ring)) {
                slot_menu_m_ = m; slot_menu_pos_ = pos;
                slot_ctl_center(m, pos, slot_menu_x_, slot_menu_y_);   // anchor on the knob
                slot_menu_hover_ = -98; slot_menu_hover_badge_ = 0;
                ensure_animating(); request_repaint();
            }
        };
        // Tag colours: the JUCE rotating palette, by the (alphabetical) tag index.
        static const char* PAL[] = {"#3498db", "#27ae60", "#e67e22", "#9b59b6",
                                    "#dc3545", "#1abc9c", "#e91e63", "#607d8b"};
        for (size_t i = 0; i < tags_.size(); ++i)
            tag_colors_.push_back(PAL[i % (sizeof(PAL) / sizeof(PAL[0]))]);
        module_fx_[0] = "LFO"; init_delay_values(0);   // LFO slot has an advanced page
        const char* fxName = std::getenv("FX_FX");   // effect to load in headless hooks
        const std::string fxn = fxName ? fxName : "Delay";
        if (std::getenv("FX_SLOTS")) {   // headless: load <FX_FX> into slots 1-3 (overlay closed)
            for (int m = 1; m <= 3; ++m) { module_fx_[(size_t)m] = fxn; init_delay_values(m); }
            if (std::getenv("FX_MENU")) { slot_menu_m_ = 1; slot_menu_pos_ = 0; slot_ctl_center(1, 0, slot_menu_x_, slot_menu_y_); }
            if (const char* fdrag = std::getenv("FX_DRAG")) {   // headless: a mid-drag reorder state
                reorder_cand_ = std::atoi(fdrag); reorder_active_ = true;
                ghost_dxs_ = ghost_dxk_ = 95.0f; scale_ = REORDER_SCALE;
                drop_target_ = std::min(6, reorder_cand_ + 1);
                if (drop_target_ != reorder_cand_) slide_[drop_target_] = -1.0f;
            }
            if (const char* w = std::getenv("FX_WIGGLE")) { wiggle_module_ = std::atoi(w); anim_clock_ = 0.13f; }
            if (const char* th = std::getenv("FX_TITLEHOVER")) { slot_title_hover_ = std::atoi(th); anim_clock_ = 0.9f; }
            if (const char* bp = std::getenv("FX_BYPASS")) { const int bh = bypass_hit(std::atoi(bp)); if (bh >= 0) hits_[(size_t)bh].on = false; }
            if (const char* sw = std::getenv("FX_SWAP")) { swap_anim_m_ = std::atoi(sw); if (const char* st = std::getenv("FX_SWAPT")) swap_anim_t_ = (float)std::atof(st); else swap_anim_t_ = 0.5f; }
            if (const char* eh = std::getenv("FX_EMPTYHOVER")) { const int e = std::atoi(eh); slot_title_hover_ = e; slot_hover_t_[e] = 1.0f; }
        }
        if (const char* fo = std::getenv("FX_OPEN")) {   // headless verify hook
            fx_menu_module_ = std::atoi(fo); fx_alpha_ = 1.0f;
            if (const char* fh = std::getenv("FX_HOVER")) {   // force a hovered cell, fully grown
                fx_hover_ = std::atoi(fh);
                if (fx_hover_ >= 0 && fx_hover_ < 12) fx_cell_t_[fx_hover_] = 1.0f;
            }
            if (std::getenv("FX_CTRL")) {   // headless: open the <FX_FX> Controls view
                module_fx_[(size_t)fx_menu_module_] = fxn; fx_view_ = 1; init_delay_values(fx_menu_module_);
                if (const char* ch = std::getenv("FX_CTLHOVER")) { ctl_hover_ = std::atoi(ch); ctl_hover_opt_ = 0; }
                if (std::getenv("FX_BTN")) { close_t_ = remove_t_ = 1.0f; }   // force button hover render
            }
        }
    }

    const std::string& tag_color(const std::string& tag) const {
        for (size_t i = 0; i < tags_.size(); ++i) if (tags_[i] == tag) return tag_colors_[i];
        static const std::string fallback = "#a19b92";
        return fallback;
    }

    void paint(cv::Canvas& canvas) override {
        vw::DesignFrameView::paint(canvas);
        const auto t = panel_transform(local_bounds());
        if (t.scale <= 0.0f) return;

        // Bypassed module: desaturate its knob's triangle (the accent loses its
        // colour) BEFORE the dim, so the grey needle then fades with the column.
        for (const auto& h : hits_) {
            if (!h.toggle || h.on || h.knob_index < 0 || h.knob_index >= (int)knobs_.size())
                continue;
            draw_mini(canvas, t, needle_svg(knobs_[(size_t)h.knob_index], h.knob_index, 2));
        }

        // Bypassed modules (dot OFF): fade each dim rect toward the panel by the
        // same amount — slot, dot, knob and labels all dim together (JUCE bypass).
        const cv::Color fade = hex_color(PANEL_BG).with_alpha(BYPASS_DIM);
        for (const auto& h : hits_) {
            if (!h.toggle || h.on) continue;
            canvas.set_fill_color(fade);
            for (const auto& r : h.dim)
                canvas.fill_rect(t.ox + r.x0 * t.scale, t.oy + r.y0 * t.scale,
                                 (r.x1 - r.x0) * t.scale, (r.y1 - r.y0) * t.scale);
        }

        // Hover: brighten the hovered knob's ring (drawn on top, exact alignment).
        if (hover_knob_ >= 0 && hover_knob_ < (int)knobs_.size())
            overlay_brighten_ring(canvas, t, knobs_[(size_t)hover_knob_], hover_knob_);
        // Hover: a disabled (bypassed) dot lights up GREY, not the accent — drawn
        // crisp over the faded column. Everything else brightens its glyph in place.
        if (hover_hit_ >= 0 && hover_hit_ < (int)hits_.size()) {
            const auto& h = hits_[(size_t)hover_hit_];
            if (h.toggle && !h.on) {
                canvas.set_fill_color(hex_color(brighten_hex(desaturate_hex(CONTROL_ACCENT))));
                canvas.fill_circle(t.ox + h.cx * t.scale, t.oy + h.cy * t.scale, 4.6f * t.scale);
            } else {
                overlay_brighten_box(canvas, t, h.cx - h.hw, h.cy - h.hh, h.cx + h.hw, h.cy + h.hh);
            }
        }

        // Header pill (purple) + live preset name, where the baked glyphs were
        // stripped. The pill shows when the browser is open or the header hovered.
        if (dropdown_open_ || hover_header_) draw_header_pill(canvas, t);
        draw_preset_name(canvas, t);

        if (radio_.ok) {
            // Selected radio option: filled dot drawn over the (un-baked) rings.
            if (radio_.sel >= 0 && radio_.sel < (int)radio_.cys.size())
                draw_radio_dot(canvas, t, radio_.sel, false);
            // Hovered, non-selected option: brightened ring outline as the affordance.
            if (hover_radio_ >= 0 && hover_radio_ != radio_.sel)
                draw_radio_dot(canvas, t, hover_radio_, true);
        }

        // The module band is one seamless surface (cards merged); content on top.
        draw_band_bg(canvas, t);
        draw_dividers(canvas, t);
        if (land_glow_m_ >= 1 && land_glow_m_ <= 6 && land_glow_t_ > 0.01f) {   // dropped-slot bg glow
            const int gm = land_glow_m_;
            // A solid core at the lift colour (so there's NO brightness jump when the
            // module lands) plus larger, fainter halo layers for a soft/diffuse edge —
            // the whole thing fades back to the band colour over the long settle.
            const float gw = SLOT_X1[gm] - SLOT_X0[gm], gx = slot_center(gm), gcy = 298.0f, gh = 442.0f - 154.0f;
            const struct { float k, a; } layers[] = {{1.14f, 0.30f}, {1.06f, 0.55f}, {1.0f, 1.0f}};
            for (const auto& ly : layers) {
                const float w = gw * ly.k, h = gh * ly.k;
                canvas.set_fill_color(hex_color("#efe9de").with_alpha(land_glow_t_ * ly.a));
                canvas.fill_rounded_rect(t.ox + (gx - w * 0.5f) * t.scale, t.oy + (gcy - h * 0.5f) * t.scale,
                                         w * t.scale, h * t.scale, 26.0f * t.scale);
            }
        }
        bool reAnim = false;
        if (reorder_active_) reAnim = reorder_update();   // may finalize → clears reorder_active_
        const bool RE = reorder_active_;
        const int src = reorder_cand_;
        // Clear the moving modules' macro-knob bands before redrawing them shifted.
        if (RE) for (int p = 1; p <= 6; ++p)
            if (std::abs(reorder_knob_dx(p)) > 0.5f) cover_knob(canvas, t, p);
        // Content for every module EXCEPT the grabbed one (drawn last, on top).
        draw_module_names(canvas, t, RE ? -src : 0);   // follows via reorder_slot_dx / reorder_knob_dx
        draw_lfo_master_controls(canvas, t);
        draw_lfo_master_labels(canvas, t);
        draw_slot_controls(canvas, t, RE ? -src : 0);
        draw_module_strip_labels(canvas, t, RE ? -src : 0);
        if (RE) {
            for (int p = 1; p <= 6; ++p)               // sliding neighbours' macro knobs
                if (p != src && std::abs(reorder_knob_dx(p)) > 0.5f)
                    draw_module_knob(canvas, t, p, reorder_knob_dx(p), 1.0f, 1.0f);
            // The grabbed module on top, fully OPAQUE: shadow + raised tile + its
            // content + macro knob — so it cleanly covers whatever it's dragged over.
            reorder_under(canvas, t);
            draw_module_knob(canvas, t, src, ghost_dxk_, scale_, 1.0f);
            draw_module_names(canvas, t, src);
            draw_slot_controls(canvas, t, src);
            draw_module_strip_labels(canvas, t, src);
            if (reAnim) request_repaint();
        }
        if (settle_frames_ > 0) {     // just-dropped: redraw affected macro knobs (committed values)
            for (int p = settle_lo_; p <= settle_hi_; ++p) {
                if (p < 1 || p > 6) continue;
                cover_knob(canvas, t, p);
                draw_module_knob(canvas, t, p, 0.0f, 1.0f, 1.0f);
                draw_module_strip_labels(canvas, t, p);   // cover_knob also wiped the name — restore it
            }
            settle_frames_--; request_repaint();
        }
        draw_bypass_dim(canvas, t);   // dim bypassed modules' icon + controls + name
        if (dropdown_open_) draw_dropdown(canvas, t);
        if (fx_menu_module_ >= 0) { fx_animate(); draw_fx_overlay(canvas, t); }
        if (slot_menu_m_ >= 0) draw_slot_menu(canvas, t);   // param picker on top
    }

    void on_hover_move(vw::Point pos) override {
        const auto t = panel_transform(local_bounds());
        if (slot_menu_m_ >= 0 && t.scale > 0.0f) {   // param picker: hover lives on the badges / close-X
            int it, badge; slot_menu_hit((pos.x - t.ox) / t.scale, (pos.y - t.oy) / t.scale, it, badge);
            if (it != slot_menu_hover_ || badge != slot_menu_hover_badge_) {
                slot_menu_hover_ = it; slot_menu_hover_badge_ = badge; ensure_animating(); request_repaint();
            }
            return;
        }
        if (fx_menu_module_ >= 0 && t.scale > 0.0f) {
            const float px = (pos.x - t.ox) / t.scale, py = (pos.y - t.oy) / t.scale;
            if (fx_view_ == 1) {   // Controls view: knobs / toggles / radio / meters / close
                int opt = -1; const int ci = ctl_at(px, py, opt);
                int mh = -1;
                if (ci < 0 && fx_menu_module_ != 0) {   // LFO has no meters
                    if (in_rect(fader_track(true), px, py)) mh = 0;
                    else if (in_rect(fader_track(false), px, py)) mh = 1; }
                int closeH = -1;   // -2 = close (above input), -3 = remove (above output)
                if (ci < 0 && mh < 0) { if (in_close_btn(px, py)) closeH = -2;
                                        else if (fx_menu_module_ != 0 && in_remove_btn(px, py)) closeH = -3; }
                if (ci != ctl_hover_ || opt != ctl_hover_opt_ || mh != meter_hover_ || closeH != fx_hover_) {
                    ctl_hover_ = ci; ctl_hover_opt_ = opt; meter_hover_ = mh; fx_hover_ = closeH;
                    request_repaint();
                }
                return;
            }
            int h = fx_cell_at(px, py);
            if (h < 0 && in_rect(fx_close_rect(), px, py)) h = -2;   // -2 = close ✕
            if (h != fx_hover_) { fx_hover_ = h; request_repaint(); }
            return;
        }
        if (dropdown_open_ && t.scale > 0.0f) {
            update_dropdown_hover((pos.x - t.ox) / t.scale, (pos.y - t.oy) / t.scale);
            return;
        }
        int hk = -1, hr = -1, hh = -1; bool hdr = false;
        if (t.scale > 0.0f) {
            const float px = (pos.x - t.ox) / t.scale, py = (pos.y - t.oy) / t.scale;
            hdr = in_pill(px, py);
            for (size_t i = 0; i < knobs_.size(); ++i) {
                const float dx = px - knobs_[i].cx, dy = py - knobs_[i].cy;
                if (dx * dx + dy * dy <= knobs_[i].hit * knobs_[i].hit) { hk = (int)i; break; }
            }
            if (hk < 0 && !hdr) hh = hit_target_at(px, py);   // pill owns the header bar
            if (hk < 0 && hh < 0 && radio_.ok &&
                px > radio_.cx - radio_.r * 2.0f && px < radio_.cx + 120.0f) {
                float bestd = 13.0f;
                for (size_t i = 0; i < radio_.cys.size(); ++i) {
                    const float dd = std::abs(py - radio_.cys[i]);
                    if (dd < bestd) { bestd = dd; hr = (int)i; }
                }
            }
        }
        // Slot controls (in the closed module view): hover the knob/toggle/radio.
        int shm = -1, shp = -1, shr = 0, sth = -1;
        if (t.scale > 0.0f) {
            const float px = (pos.x - t.ox) / t.scale, py = (pos.y - t.oy) / t.scale;
            slot_ctl_at(px, py, shm, shp, shr);
            for (int m = 1; m <= 6; ++m) {   // loaded: title zone → "EDIT"; empty: whole slot → "+"
                const bool empty = module_fx_[(size_t)m].empty();
                const float halfw = (SLOT_X1[m] - SLOT_X0[m]) * 0.5f;
                const bool hit = empty ? (std::abs(px - slot_center(m)) < halfw && py >= 156.0f && py <= 440.0f)
                                       : (std::abs(px - slot_center(m)) < 60.0f && py >= 160.0f && py <= 202.0f);
                if (hit) { sth = m; break; }
            }
        }
        if (shm != slot_hover_m_ || shp != slot_hover_pos_ || shr != slot_hover_ring_ || sth != slot_title_hover_) {
            slot_hover_m_ = shm; slot_hover_pos_ = shp; slot_hover_ring_ = shr; slot_title_hover_ = sth;
            if (sth >= 0) ensure_animating();   // run the "Edit..." dot animation while hovering
            request_repaint();
        }
        if (hk != hover_knob_ || hr != hover_radio_ || hh != hover_hit_ || hdr != hover_header_) {
            hover_knob_ = hk; hover_radio_ = hr; hover_hit_ = hh; hover_header_ = hdr; request_repaint();
        }
    }

    void on_mouse_leave() override {
        if (ctl_hover_ >= 0 || meter_hover_ >= 0 || slot_hover_m_ >= 0 || slot_title_hover_ >= 0) {
            ctl_hover_ = ctl_hover_opt_ = -1; meter_hover_ = -1;
            slot_hover_m_ = slot_hover_pos_ = -1; slot_hover_ring_ = 0; slot_title_hover_ = -1; request_repaint();
        }
        if (hover_knob_ >= 0 || hover_radio_ >= 0 || hover_hit_ >= 0 || hover_header_ ||
            hover_menu_ || hover_scrollbar_ || hover_search_) {
            hover_knob_ = hover_radio_ = hover_hit_ = -1;
            hover_header_ = false; hover_menu_ = false; hover_scrollbar_ = false;
            hover_search_ = false; request_repaint();
        }
    }

    // The header "pill" — the whole bar around the ‹ › arrows, name and dice.
    static bool in_pill(float px, float py) {
        return px > 452.0f && px < 820.0f && py > 77.0f && py < 107.0f;
    }

    // Which effects have a ported advanced-controls page (any with a table).
    static bool effect_has_controls(const std::string& fx) { return !effect_controls(fx).empty(); }

    // The control table for module m's loaded effect.
    const std::vector<FxControl>& mod_ctls(int m) const {
        return effect_controls(m >= 0 && m < 8 ? module_fx_[(size_t)m] : std::string());
    }

    // Re-point the open overlay at module `m` WITHOUT re-animating (JUCE switchToSlot):
    // its loaded effect → Controls view, an empty "---" → the icon picker.
    void switch_overlay_to(int m) {
        fx_menu_module_ = m; fx_hover_ = -1;
        for (float& tt : fx_cell_t_) tt = 0.0f;
        fx_view_ = effect_has_controls(module_fx_[(size_t)m]) ? 1 : 0;
        if (fx_view_ == 1) init_delay_values(fx_menu_module_);
        ctl_hover_ = ctl_hover_opt_ = -1; meter_hover_ = -1;
        request_repaint();   // keep fx_alpha_ as-is (no fade) — matches switchToSlot
    }

    // A click that missed the in-band controls while the overlay is open. JUCE UX:
    // the ✕ or bare background closes; another module's label switches the page to
    // it; a macro knob stays open and passes through so it can still be dragged.
    void overlay_outside_click(float px, float py, vw::Point pos) {
        if (in_rect(fx_close_rect(), px, py)) { fx_close(); request_repaint(); return; }
        if (in_rect(fx_panel(), px, py)) { request_repaint(); return; }   // empty band → stay open
        for (int m = 1; m <= 6; ++m)   // another effect slot's "---" label → switch to it
            if (std::abs(px - knobs_[(size_t)m].cx) < 42.0f && py >= 598.0f && py <= 640.0f) {
                if (m != fx_menu_module_) switch_overlay_to(m);
                return;
            }
        for (size_t i = 0; i < knobs_.size() && i < 8; ++i) {   // a macro knob → keep open, drag it
            const float dx = px - knobs_[i].cx, dy = py - knobs_[i].cy;
            if (dx * dx + dy * dy <= knobs_[i].hit * knobs_[i].hit) {
                vw::DesignFrameView::on_mouse_down(pos); return;
            }
        }
        fx_close(); request_repaint();   // bare background → dismiss
    }

    void on_mouse_down(vw::Point pos) override {
        pre_press_wiggle_ = wiggle_module_;   // remember edit mode at press time (for click-to-exit)
        reorder_cand_ = -1;                   // start each press fresh (early returns leave it clear)
        const auto tt = panel_transform(local_bounds());
        if (tt.scale > 0.0f) {
            const float px = (pos.x - tt.ox) / tt.scale, py = (pos.y - tt.oy) / tt.scale;

            if (slot_menu_m_ >= 0) {   // param picker open: badge / row select, re-anchor, or LEAVE edit
                int item, badge; slot_menu_hit(px, py, item, badge);
                if (item == -3) { slot_menu_m_ = -1; set_wiggle_module(-1); request_repaint(); return; }  // close-X
                if (item == -99) {     // outside the popup
                    int sm, sp, sr;    // a wiggling knob/ring → re-anchor the popup there (stay in edit)
                    if (slot_ctl_at(px, py, sm, sp, sr) && sm == slot_menu_m_) {
                        slot_menu_pos_ = sp; slot_ctl_center(sm, sp, slot_menu_x_, slot_menu_y_);
                        request_repaint();
                    } else {           // anything else → leave edit mode entirely
                        slot_menu_m_ = -1; set_wiggle_module(-1); request_repaint();
                    }
                } else if (item != -98) slot_menu_click(item, badge);        // item (not chrome)
                return;
            }

            if (wiggle_module_ >= 0) {   // edit mode with the popup closed (e.g. after Swap)
                int sm, sp, sr;
                if (slot_ctl_at(px, py, sm, sp, sr) && sm == wiggle_module_) {   // a wiggling knob → reopen popup
                    slot_menu_m_ = wiggle_module_; slot_menu_pos_ = sp;
                    slot_ctl_center(sm, sp, slot_menu_x_, slot_menu_y_);
                    slot_menu_hover_ = -98; slot_menu_hover_badge_ = 0;
                    request_repaint(); return;
                }
                set_wiggle_module(-1); request_repaint(); return;   // anything else → leave edit mode
            }

            if (fx_menu_module_ >= 0 && !fx_closing_) {   // FX overlay open
                if (fx_view_ == 1) {   // Controls view: close / remove / controls / faders
                    if (in_close_btn(px, py)) { fx_close(); return; }
                    if (fx_menu_module_ != 0 && in_remove_btn(px, py)) { remove_effect(); return; }   // remove → show browser (LFO pinned)
                    int opt = -1; const int ci = ctl_at(px, py, opt);
                    if (ci >= 0) {
                        const int mm = fx_menu_module_;
                        const auto& c = mod_ctls(mm)[(size_t)ci];
                        if (c.kind == FxControl::Knob) {        // begin vertical drag
                            knob_drag_ = ci; knob_drag_ring_ = (opt == 1);
                            drag_y0_ = py; drag_v0_ = mod_val_[mm][ci]; drag_dep0_ = mod_dep_[mm][ci];
                        } else if (c.kind == FxControl::Toggle) {   // cycle the option
                            const int n = (int)c.options.size();
                            mod_val_[mm][ci] = (float)(((int)mod_val_[mm][ci] + 1) % n);
                        } else if (opt >= 0) {                  // pick the radio option
                            mod_val_[mm][ci] = (float)opt;
                        }
                        request_repaint(); return;
                    }
                    if (fx_menu_module_ == 0) {   // LFO depth sliders (cols 4–6)
                        int row, handle;
                        if (lfo_depth_at(px, py, row, handle)) {
                            lfo_depth_drag_ = row; lfo_depth_handle_ = handle;
                            lfo_depth_drag_to(px); request_repaint(); return;
                        }
                    }
                    if (fx_menu_module_ != 0)   // LFO has no input/output faders
                    for (int f = 0; f < 2; ++f) {
                        if (in_rect(fader_track(f == 0), px, py)) {
                            fader_drag_ = f;
                            (f == 0 ? input_gain_ : output_gain_) = fader_value_from_y(py, f == 0);
                            request_repaint(); return;
                        }
                    }
                    overlay_outside_click(px, py, pos); return;
                }
                const int cell = fx_cell_at(px, py);
                if (cell >= 0) {   // pick an effect → go straight to its advanced page
                    const std::string& label = fx_types_[(size_t)cell].label;
                    module_fx_[(size_t)fx_menu_module_] = label;
                    if (effect_has_controls(label)) {
                        fx_view_ = 1; init_delay_values(fx_menu_module_);
                        ctl_hover_ = ctl_hover_opt_ = -1; meter_hover_ = -1;
                    } else {
                        fx_close();   // no controls ported for this effect yet
                    }
                    request_repaint(); return;
                }
                overlay_outside_click(px, py, pos);
                return;
            }

            if (dropdown_open_) {
                if (in_rect(scroll_thumb_, px, py)) {       // grab the scrollbar thumb
                    dragging_scroll_ = true; drag_scroll_y0_ = py; drag_scroll_s0_ = scroll_;
                    return;
                }
                if (handle_dropdown_click(px, py)) return;
                // A click on the nav buttons navigates and keeps the panel open;
                // anything else outside the panel closes it.
                const int hb = hit_target_at(px, py);
                if (hb >= 0 && !hits_[(size_t)hb].toggle) {
                    if (hits_[(size_t)hb].id == "preset_prev") preset_step(-1);
                    else if (hits_[(size_t)hb].id == "preset_next") preset_step(+1);
                    else if (hits_[(size_t)hb].id == "dice") preset_random();
                    return;
                }
                if (in_pill(px, py)) { dropdown_open_ = false; request_repaint(); return; }
                if (!in_rect({DD_X0, DD_Y0, DD_X1, DD_Y1}, px, py)) {
                    dropdown_open_ = false; request_repaint(); return;
                }
                return;   // click inside panel chrome (no-op) — stay open
            }

            const int h = hit_target_at(px, py);
            if (h >= 0) {
                auto& target = hits_[(size_t)h];
                if (target.toggle) { target.on = !target.on; request_repaint(); }
                else if (target.id == "preset_prev") preset_step(-1);
                else if (target.id == "preset_next") preset_step(+1);
                else if (target.id == "dice") preset_random();
                // gear: settings — no backing action in the playground yet.
                return;
            }
            // The header pill (name area, not a button) opens the browser.
            if (in_pill(px, py) && !presets_.empty()) {
                dropdown_open_ = true; search_focused_ = false; request_repaint(); return;
            }
        }
        if (radio_.ok) {
            const auto t = panel_transform(local_bounds());
            if (t.scale > 0.0f) {
                const float px = (pos.x - t.ox) / t.scale, py = (pos.y - t.oy) / t.scale;
                // Clickable region = the whole option row (circle + its text label
                // to the right), matching JUCE's row-wide radio hit zone.
                if (px > radio_.cx - radio_.r * 2.0f && px < radio_.cx + 120.0f) {
                    int best = -1; float bestd = 13.0f;   // < half the 22px row pitch
                    for (size_t i = 0; i < radio_.cys.size(); ++i) {
                        const float dd = std::abs(py - radio_.cys[i]);
                        if (dd < bestd) { bestd = dd; best = (int)i; }
                    }
                    if (best >= 0) {
                        if (radio_.sel != best) { radio_.sel = best; request_repaint(); }
                        return;
                    }
                }
            }
        }
        // FX reorder: pressing a middle module's slot (1-6) arms a reorder drag —
        // grab it by its "---" name to drag the whole module. LFO/MASTER pinned.
        // Slot controls (module view): a hit adjusts the control instead of
        // arming a reorder/open — knob drag, toggle cycle, radio pick.
        if (!dropdown_open_ && !reorder_active_ && fx_menu_module_ < 0) {
            const auto t = panel_transform(local_bounds());
            if (t.scale > 0.0f) {
                const float px = (pos.x - t.ox) / t.scale, py = (pos.y - t.oy) / t.scale;
                int sm, sp, sr;
                if (slot_ctl_at(px, py, sm, sp, sr)) {
                    if (wiggle_module_ == sm) {   // edit mode: clicking a control opens the assignment popup
                        slot_menu_m_ = sm; slot_menu_pos_ = sp;
                        slot_menu_hover_ = -98; slot_menu_hover_badge_ = 0;
                        slot_ctl_center(sm, sp, slot_menu_x_, slot_menu_y_);   // anchor on the knob
                        request_repaint(); return;
                    }
                    if (wiggle_module_ >= 0 && sm != wiggle_module_) set_wiggle_module(-1);  // another module's control → exit edit
                    const int ci = slot_vis_[sm][sp];
                    const auto& c = mod_ctls(sm)[(size_t)ci];
                    if (c.kind == FxControl::Knob) {
                        slot_drag_m_ = sm; slot_drag_pos_ = sp; slot_drag_ring_ = (sr == 1);
                        drag_y0_ = py; drag_v0_ = mod_val_[sm][ci]; drag_dep0_ = mod_dep_[sm][ci];
                    } else if (c.kind == FxControl::Toggle) {
                        mod_val_[sm][ci] = (float)(((int)mod_val_[sm][ci] + 1) % (int)c.options.size());
                    } else {
                        mod_val_[sm][ci] = (float)sr;
                    }
                    request_repaint(); return;
                }
            }
        }
        // LFO slot (0): a pinned modulator — pressing anywhere in its band arms an
        // open of its advanced page (no reorder/wiggle, unlike slots 1–6).
        lfo_press_ = false;
        if (!dropdown_open_ && !reorder_active_ && fx_menu_module_ < 0) {
            const auto t = panel_transform(local_bounds());
            if (t.scale > 0.0f) {
                const float px = (pos.x - t.ox) / t.scale, py = (pos.y - t.oy) / t.scale;
                if (px >= SLOT_X0[0] && px <= SLOT_X1[0] && py >= 154.0f && py <= 442.0f) {
                    lfo_press_ = true;
                    vw::DesignFrameView::on_mouse_down(pos);
                    return;
                }
            }
        }
        reorder_cand_ = -1;
        if (!dropdown_open_ && !reorder_active_) {
            const auto t = panel_transform(local_bounds());
            if (t.scale > 0.0f) {
                const float px = (pos.x - t.ox) / t.scale, py = (pos.y - t.oy) / t.scale;
                for (int i = 1; i <= 6; ++i) {
                    const bool on_slot = px >= SLOT_X0[i] && px <= SLOT_X1[i] && py >= 154.0f && py <= 442.0f;
                    const bool on_label = std::abs(px - knobs_[(size_t)i].cx) < 42.0f && py >= 598.0f && py <= 640.0f;
                    if (on_slot || on_label) {
                        reorder_cand_ = i; reorder_active_ = false;
                        press_on_label_ = on_label && !on_slot;   // bottom name → advanced; icon → Edit/wiggle
                        reorder_sx_ = px; reorder_sy_ = py;
                        break;
                    }
                }
            }
        }
        if (reorder_cand_ < 0) set_wiggle_module(-1);   // clicking off any module exits edit/wiggle mode (eased)
        vw::DesignFrameView::on_mouse_down(pos);
    }

    void on_mouse_drag(vw::Point pos) override {
        if (slot_drag_m_ >= 0) {   // dragging a slot knob (value or ring)
            const auto t = panel_transform(local_bounds());
            if (t.scale > 0.0f) {
                const float py = (pos.y - t.oy) / t.scale;
                const float delta = (drag_y0_ - py) * KNOB_DRAG_SENS;
                const int ci = slot_vis_[slot_drag_m_][slot_drag_pos_];
                if (slot_drag_ring_)
                    mod_dep_[slot_drag_m_][ci] = std::clamp(drag_dep0_ + delta, -1.0f, 1.0f);
                else
                    mod_val_[slot_drag_m_][ci] = std::clamp(drag_v0_ + delta, 0.0f, 1.0f);
                request_repaint();
            }
            return;
        }
        if (knob_drag_ >= 0) {   // dragging a control knob (vertical: up = more)
            const auto t = panel_transform(local_bounds());
            if (t.scale > 0.0f) {
                const float py = (pos.y - t.oy) / t.scale;
                const float delta = (drag_y0_ - py) * KNOB_DRAG_SENS;
                const int mm = fx_menu_module_;
                if (knob_drag_ring_)   // ring drag sets bipolar mod depth (−1..1)
                    mod_dep_[mm][knob_drag_] = std::clamp(drag_dep0_ + delta, -1.0f, 1.0f);
                else                   // value drag sets the base value (0..1)
                    mod_val_[mm][knob_drag_] = std::clamp(drag_v0_ + delta, 0.0f, 1.0f);
                request_repaint();
            }
            return;
        }
        if (fader_drag_ >= 0) {   // dragging an input/output meter handle
            const auto t = panel_transform(local_bounds());
            if (t.scale > 0.0f) {
                const float py = (pos.y - t.oy) / t.scale;
                (fader_drag_ == 0 ? input_gain_ : output_gain_) = fader_value_from_y(py, fader_drag_ == 0);
                request_repaint();
            }
            return;
        }
        if (lfo_depth_drag_ >= 0) {   // dragging an LFO depth-slider handle (horizontal)
            const auto t = panel_transform(local_bounds());
            if (t.scale > 0.0f) { lfo_depth_drag_to((pos.x - t.ox) / t.scale); request_repaint(); }
            return;
        }
        if (dragging_scroll_) {
            const auto t = panel_transform(local_bounds());
            if (t.scale > 0.0f) {
                const float py = (pos.y - t.oy) / t.scale;
                float maxscroll, travel; scroll_metrics(maxscroll, travel);
                scroll_ = std::clamp(drag_scroll_s0_ + (py - drag_scroll_y0_) * maxscroll / travel,
                                     0.0f, maxscroll);
                request_repaint();
            }
            return;
        }
        if (reorder_cand_ >= 0 && !dropping_) {
            const auto t = panel_transform(local_bounds());
            if (t.scale > 0.0f) {
                const float px = (pos.x - t.ox) / t.scale, py = (pos.y - t.oy) / t.scale;
                if (!reorder_active_) {
                    const float dx = px - reorder_sx_, dy = py - reorder_sy_;
                    if (dx * dx + dy * dy > 25.0f) {
                        reorder_active_ = true;
                        land_glow_m_ = -1; land_glow_t_ = 0.0f;   // clear any leftover settle glow
                        ensure_animating();
                    }
                }
                if (reorder_active_) {
                    ghost_dxs_ = ghost_dxk_ = px - reorder_sx_;   // rigid module follows cursor
                    update_drop_target();
                    request_repaint();
                    return;
                }
            }
        }
        vw::DesignFrameView::on_mouse_drag(pos);
    }

    void on_mouse_up(vw::Point pos) override {
        if (lfo_press_) {   // released inside the LFO slot → open its advanced page
            lfo_press_ = false;
            set_wiggle_module(-1);
            fx_menu_module_ = 0; fx_hover_ = -1;
            fx_view_ = 1; init_delay_values(0);
            ctl_hover_ = -1; ctl_hover_opt_ = -1; meter_hover_ = -1;
            for (float& tt : fx_cell_t_) tt = 0.0f;
            fx_closing_ = false; fx_alpha_ = 0.0f; ensure_animating();
            request_repaint();
            vw::DesignFrameView::on_mouse_up(pos);
            return;
        }
        if (slot_drag_m_ >= 0) { slot_drag_m_ = slot_drag_pos_ = -1; return; }
        if (knob_drag_ >= 0) { knob_drag_ = -1; return; }
        if (fader_drag_ >= 0) { fader_drag_ = -1; return; }
        if (lfo_depth_drag_ >= 0) { lfo_depth_drag_ = -1; return; }
        if (dragging_scroll_) { dragging_scroll_ = false; return; }
        if (reorder_active_ && !dropping_) {
            update_drop_target();        // begin the ease-in-out drop (draw_reorder finalizes)
            dropping_ = true; drop_t_ = 0.0f;
            drop_dxs0_ = ghost_dxs_; drop_dxs1_ = slot_center(drop_target_) - slot_center(reorder_cand_);
            drop_dxk0_ = ghost_dxk_; drop_dxk1_ = knobs_[(size_t)drop_target_].cx - knobs_[(size_t)reorder_cand_].cx;
            drop_s0_ = scale_;
            ensure_animating(); request_repaint();
            vw::DesignFrameView::on_mouse_up(pos);
            return;
        }
        if (reorder_cand_ >= 0) {   // a press that didn't turn into a drag
            const int m = reorder_cand_;
            const bool loaded = !module_fx_[(size_t)m].empty();
            if (loaded && !press_on_label_) {
                // Click a loaded effect's ICON ("EDIT"): enter wiggle — unless this is the
                // module that was already wiggling (then it's a toggle-off / exit).
                const int target = (m == pre_press_wiggle_) ? -1 : m;
                set_wiggle_module(target);
                if (target >= 0) {   // entering edit mode → show the assignment popup immediately
                    slot_menu_m_ = target; slot_menu_pos_ = 0;
                    slot_ctl_center(target, 0, slot_menu_x_, slot_menu_y_);
                    slot_menu_hover_ = -98; slot_menu_hover_badge_ = 0;
                } else {
                    slot_menu_m_ = -1;
                }
            } else {
                // Empty slot, or the bottom NAME → open the FX overlay (picker / advanced).
                set_wiggle_module(-1);
                fx_menu_module_ = m; fx_hover_ = -1;
                fx_view_ = effect_has_controls(module_fx_[(size_t)m]) ? 1 : 0;
                if (fx_view_ == 1) init_delay_values(fx_menu_module_);
                ctl_hover_ = -1; ctl_hover_opt_ = -1; meter_hover_ = -1;
                for (float& tt : fx_cell_t_) tt = 0.0f;   // start with every icon at rest
                fx_closing_ = false; fx_alpha_ = 0.0f; ensure_animating();
            }
            reorder_cand_ = -1; request_repaint();
        }
        vw::DesignFrameView::on_mouse_up(pos);
    }

private:
    // Index of the hit target (icon button / bypass dot) under (px,py) in panel
    // coords, or -1. Boxes are tiny, so a simple containment test is enough.
    int hit_target_at(float px, float py) const {
        for (size_t i = 0; i < hits_.size(); ++i) {
            const auto& h = hits_[i];
            if (std::abs(px - h.cx) <= h.hw && std::abs(py - h.cy) <= h.hh) return (int)i;
        }
        return -1;
    }

    // Brighten every #hex-filled path whose first point is inside the box, by
    // re-drawing those paths lightened into a mini-SVG on top — the generic
    // (any-colour) sibling of overlay_brighten_ring, used for icon buttons/dots.
    void overlay_brighten_box(cv::Canvas& canvas, const PanelTransform& t,
                              float x0, float y0, float x1, float y1) const {
        std::string inner;
        for (size_t p = svg_copy_.find("<path "); p != std::string::npos;
             p = svg_copy_.find("<path ", p + 1)) {
            const size_t end = svg_copy_.find("/>", p);
            if (end == std::string::npos) break;
            std::string el = svg_copy_.substr(p, end - p + 2);
            const size_t fp = el.find("fill=\"#");
            if (fp == std::string::npos) continue;
            float x, y;
            if (!first_move(el, x, y) || x < x0 || x > x1 || y < y0 || y > y1) continue;
            const size_t he = el.find('"', fp + 6);
            const std::string hex = el.substr(fp + 6, he - (fp + 6));
            el.replace(fp + 6, hex.size(), brighten_hex(hex));
            inner += el;
        }
        if (inner.empty()) return;
        char hdr[160];
        std::snprintf(hdr, sizeof(hdr),
                      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %.0f %.0f\">",
                      svg_w_, svg_h_);
        const std::string mini = std::string(hdr) + inner + "</svg>";
        canvas.draw_svg(mini, t.ox, t.oy, svg_w_ * t.scale, svg_h_ * t.scale);
    }

    // Re-draw every #hex-filled path in the box recoloured to `hex` (used to flip
    // the baked ‹ › arrows and dice to white over the active header pill).
    void overlay_recolor_box(cv::Canvas& canvas, const PanelTransform& t,
                             float x0, float y0, float x1, float y1, const std::string& hex) const {
        std::string inner;
        for (size_t p = svg_copy_.find("<path "); p != std::string::npos;
             p = svg_copy_.find("<path ", p + 1)) {
            const size_t end = svg_copy_.find("/>", p);
            if (end == std::string::npos) break;
            std::string el = svg_copy_.substr(p, end - p + 2);
            const size_t fp = el.find("fill=\"#");
            if (fp == std::string::npos) continue;
            float x, y;
            if (!first_move(el, x, y) || x < x0 || x > x1 || y < y0 || y > y1) continue;
            const size_t he = el.find('"', fp + 6);
            el.replace(fp + 6, he - (fp + 6), hex);
            inner += el;
        }
        if (inner.empty()) return;
        char hdr[160];
        std::snprintf(hdr, sizeof(hdr),
                      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %.0f %.0f\">",
                      svg_w_, svg_h_);
        canvas.draw_svg(std::string(hdr) + inner + "</svg>", t.ox, t.oy,
                        svg_w_ * t.scale, svg_h_ * t.scale);
    }

    // The header "pill" — a purple rounded fill behind ‹ › / name / dice when the
    // browser is open or the header is hovered, with those glyphs flipped white.
    void draw_header_pill(cv::Canvas& g, const PanelTransform& t) const {
        const float x0 = 452, x1 = 820, y0 = 77, y1 = 107;
        g.set_fill_color(hex_color(dropdown_open_ ? "#7b6896" : brighten_hex("#7b6896")));
        g.fill_rounded_rect(t.ox + x0 * t.scale, t.oy + y0 * t.scale,
                            (x1 - x0) * t.scale, (y1 - y0) * t.scale, (y1 - y0) * 0.5f * t.scale);
        overlay_recolor_box(g, t, 466, 78, 526, 106, "#ffffff");   // ‹ ›
        overlay_recolor_box(g, t, 772, 76, 808, 108, "#ffffff");   // dice
    }

    // Draw the #a19b92 radio marker for option `i`: filled when selected,
    // brightened ring outline when used as the hover affordance.
    void draw_radio_dot(cv::Canvas& canvas, const PanelTransform& t, int i, bool hover) const {
        const float vx = t.ox + radio_.cx * t.scale;
        const float vy = t.oy + radio_.cys[(size_t)i] * t.scale;
        const auto base = hover ? brighten_hex("#a19b92") : std::string("#a19b92");
        const int r = (int)std::strtol(base.substr(1, 2).c_str(), nullptr, 16);
        const int g = (int)std::strtol(base.substr(3, 2).c_str(), nullptr, 16);
        const int b = (int)std::strtol(base.substr(5, 2).c_str(), nullptr, 16);
        // Selected → solid #a19b92; hover → solid brightened (a fill preview of
        // what selecting it will look like), matching the knob ring's lighten.
        canvas.set_fill_color(cv::Color::rgba8((uint8_t)r, (uint8_t)g, (uint8_t)b));
        canvas.fill_circle(vx, vy, radio_.r * t.scale);
    }

    // Build a mini-SVG of just a knob's needle at its LIVE rotation, recoloured —
    // brightened (hover) or desaturated to grey (bypassed). The baked static-
    // rotate <g> is kept and an outer value-rotate <g> added; both about (cx,cy)
    // so they commute and add, landing exactly on the base needle. "" if absent.
    // recolor: 0 = keep original, 1 = brighten (hover), 2 = grey (bypass).
    std::string needle_svg(const KnobHover& k, int index, int recolor) const {
        if (k.needle_id.empty()) return "";
        const size_t idp = svg_copy_.find(k.needle_id);
        if (idp == std::string::npos) return "";
        const size_t gopen = svg_copy_.rfind("<g ", idp);
        const size_t gclose = svg_copy_.find("</g>", idp);
        if (gopen == std::string::npos || gclose == std::string::npos) return "";
        std::string block = svg_copy_.substr(gopen, gclose + 4 - gopen);
        const size_t nf = block.find("fill=\"#");
        if (recolor != 0 && nf != std::string::npos) {
            const size_t ne = block.find('"', nf + 6);
            const std::string nhex = block.substr(nf + 6, ne - (nf + 6));
            block.replace(nf + 6, nhex.size(), recolor == 2 ? desaturate_hex(nhex) : brighten_hex(nhex));
        }
        const float val = (float)element_value(index);
        char wrap[96];
        std::snprintf(wrap, sizeof(wrap), "<g transform=\"rotate(%.3f %.3f %.3f)\">",
                      (val - 0.5f) * 270.0f, k.cx, k.cy);
        return std::string(wrap) + block + "</g>";
    }

    // Draw a mini-SVG (paths in panel coords) on top at the panel transform.
    void draw_mini(cv::Canvas& canvas, const PanelTransform& t, const std::string& inner) const {
        if (inner.empty()) return;
        char hdr[160];
        std::snprintf(hdr, sizeof(hdr),
                      "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %.0f %.0f\">",
                      svg_w_, svg_h_);
        canvas.draw_svg(std::string(hdr) + inner + "</svg>", t.ox, t.oy,
                        svg_w_ * t.scale, svg_h_ * t.scale);
    }

    // ── FX reorder (drag macro knobs 1-6) ───────────────────────────────────
    // Paths whose first point is in the box, EXCLUDING the triangle needle
    // (#7b6896 with no bezier) — that's drawn separately so it keeps its rotation.
    std::string band_paths(float x0, float y0, float x1, float y1) const {
        std::string inner;
        for (size_t p = svg_copy_.find("<path "); p != std::string::npos; p = svg_copy_.find("<path ", p + 1)) {
            const size_t end = svg_copy_.find("/>", p);
            if (end == std::string::npos) break;
            const std::string el = svg_copy_.substr(p, end - p + 2);
            float x, y;
            if (!first_move(el, x, y) || x < x0 || x > x1 || y < y0 || y > y1) continue;
            if (el.find("fill=\"#") == std::string::npos) continue;   // skip no-fill (defaults to black)
            if (el.find("fill=\"#7b6896\"") != std::string::npos && el.find('C') == std::string::npos) continue;
            inner += el;
        }
        return inner;
    }

    float slot_center(int i) const { return (SLOT_X0[i] + SLOT_X1[i]) * 0.5f; }

    // Horizontal offset of a module's SLOT-band content during a reorder (0 at
    // rest): the grabbed module follows the cursor; neighbours slide a slot pitch.
    // A sliding neighbour moves by the EXACT centre spacing in its slide direction
    // (slot centres aren't perfectly even, so a fixed pitch left a ~½px nudge on drop).
    float reorder_slot_dx(int m) const {
        if (!reorder_active_ || m < 1 || m > 6) return 0.0f;
        if (m == reorder_cand_) return ghost_dxs_;
        const float s = slide_[m];
        if (s == 0.0f) return 0.0f;
        const float pitch = (s < 0.0f) ? (slot_center(m) - slot_center(m - 1))
                                       : (slot_center(m + 1) - slot_center(m));
        return s * pitch;
    }
    float reorder_knob_dx(int m) const {   // macro-knob / bottom-label offset (exact knob spacing)
        if (!reorder_active_ || m < 1 || m > 6) return 0.0f;
        if (m == reorder_cand_) return ghost_dxk_;
        const float s = slide_[m];
        if (s == 0.0f) return 0.0f;
        const float pitch = (s < 0.0f) ? (knobs_[(size_t)m].cx - knobs_[(size_t)(m - 1)].cx)
                                       : (knobs_[(size_t)(m + 1)].cx - knobs_[(size_t)m].cx);
        return s * pitch;
    }
    // iPhone-style remove badge: a small dark circle with a "−", upper-left of a
    // wiggling control (cx,cy = the control's already-wiggled centre).
    void draw_remove_badge(cv::Canvas& g, const PanelTransform& t, float cx, float cy, float outerR) const {
        auto X = [&](float v) { return t.ox + v * t.scale; };
        auto Y = [&](float v) { return t.oy + v * t.scale; };
        auto S = [&](float v) { return v * t.scale; };
        const float bx = cx - outerR * 0.707f, by = cy - outerR * 0.707f, r = 9.0f;   // upper-left of the ring
        g.set_fill_color(hex_color(brighten_hex(CONTROL_LABEL)));   // knob-stroke brown, slightly lighter
        g.fill_circle(X(bx), Y(by), S(r));
        g.set_stroke_color(hex_color("#f5f1ed"));
        g.set_line_width(S(2.0f)); g.set_line_cap(cv::LineCap::round);
        g.begin_path(); g.move_to(X(bx - r * 0.45f), Y(by)); g.line_to(X(bx + r * 0.45f), Y(by)); g.stroke_current_path();
    }

    // iPhone-style "edit mode" wiggle: rotate a point a few degrees back-and-forth
    // about module m's slot-band centre (no-op unless m is the wiggling module).
    void wiggle_xy(int m, float& x, float& y) const {
        float amp = (wiggle_module_ == m) ? 1.0f : (wiggle_fade_m_ == m ? wiggle_fade_ : 0.0f);
        if (amp <= 0.001f) return;
        const float a = std::sin(anim_clock_ * 8.0f) * 0.045f * amp;   // ±~2.6°, scaled by the (fading) amplitude
        const float cx = slot_center(m), cy = 298.0f, dx = x - cx, dy = y - cy;
        x = cx + dx * std::cos(a) - dy * std::sin(a);
        y = cy + dx * std::sin(a) + dy * std::cos(a);
    }
    // Enter/leave wiggle edit mode; leaving (or switching) eases the old module out.
    void set_wiggle_module(int m) {
        if (m == wiggle_module_) return;
        if (wiggle_module_ >= 0) { wiggle_fade_m_ = wiggle_module_; wiggle_fade_ = 1.0f; }  // settle the old one
        wiggle_module_ = m;
        ensure_animating();
    }

    // Erase a whole module (slot panel above + knob column below) from its rest.
    void cover_module(cv::Canvas& canvas, const PanelTransform& t, int i) const {
        const float kx = knobs_[(size_t)i].cx;
        canvas.set_fill_color(hex_color("#fbf4e6"));
        canvas.fill_rect(t.ox + SLOT_X0[i] * t.scale, t.oy + 154 * t.scale,
                         (SLOT_X1[i] - SLOT_X0[i]) * t.scale, (442 - 154) * t.scale);   // slot
        canvas.fill_rect(t.ox + (kx - 61.4f) * t.scale, t.oy + 452 * t.scale,
                         (117.4f) * t.scale, (636 - 452) * t.scale);                    // knob band
    }

    // Draw a whole module shifted (slot by dxs, knob by dxk) and scaled about each
    // group's centre by `s` (so a grabbed module grows "toward the user"), at alpha.
    void draw_module(cv::Canvas& canvas, const PanelTransform& t, int i,
                     float dxs, float dxk, float s, float alpha) const {
        const float kx = knobs_[(size_t)i].cx;
        auto group = [&](float dx, float cx, float cy, const std::string& paths) {
            char w[224];
            std::snprintf(w, sizeof(w),
                "<g transform=\"translate(%.2f 0) translate(%.2f %.2f) scale(%.4f) translate(%.2f %.2f)\" opacity=\"%.3f\">",
                dx, cx, cy, s, -cx, -cy, alpha);
            draw_mini(canvas, t, std::string(w) + paths + "</g>");
        };
        group(dxs, slot_center(i), 298.0f, band_paths(SLOT_X0[i], 154, SLOT_X1[i], 442));
        group(dxk, kx, 544.0f, band_paths(kx - 61.4f, 452, kx + 56.0f, 636) + needle_svg(knobs_[(size_t)i], i, 0));
    }

    // Insertion slot for the ghost = the nearest middle slot to the ghost centre.
    void update_drop_target() {
        const float gc = slot_center(reorder_cand_) + ghost_dxs_;
        int dt = reorder_cand_; float best = 1e9f;
        for (int p = 1; p <= 6; ++p) {
            const float d = std::abs(gc - slot_center(p));
            if (d < best) { best = d; dt = p; }
        }
        drop_target_ = dt;
    }

    static float ease_in_out(float x) {   // cubic
        return x < 0.5f ? 4.0f * x * x * x : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) * 0.5f;
    }

    // Keep the host's 60fps timer alive while a reorder animates (it only runs
    // while there's a frame-clock subscriber) — otherwise the drop tween stalls
    // until the next mouse event. The subscription removes itself when idle.
    void ensure_animating() {
        if (anim_sub_ >= 0) return;
        if (auto* fc = frame_clock())
            anim_sub_ = fc->subscribe([this](float dt) {
                anim_clock_ += dt;   // free-running phase for Edit-dots + wiggle
                if (wiggle_fade_m_ >= 0) {                      // ease the stopped wiggle out (damped, slow)
                    wiggle_fade_ *= std::exp(-dt * 2.3f);
                    if (wiggle_fade_ < 0.02f) { wiggle_fade_ = 0.0f; wiggle_fade_m_ = -1; }
                }
                if (swap_anim_m_ >= 0) {                        // Swap-Parameters carousel
                    swap_anim_t_ += dt / 0.68f;                 // twice as slow
                    if (swap_anim_t_ >= 1.0f) { swap_anim_t_ = 0.0f; swap_anim_m_ = -1; }
                }
                if (land_glow_t_ > 0.0f) {                      // dropped slot's bg eases back to normal
                    land_glow_t_ *= std::exp(-dt * 1.95f);      // ~2s settle
                    if (land_glow_t_ < 0.02f) { land_glow_t_ = 0.0f; land_glow_m_ = -1; }
                }
                {   // header close-X hover grow
                    const float target = (slot_menu_m_ >= 0 && slot_menu_hover_ == -3) ? 1.0f : 0.0f;
                    smenu_x_t_ += (target - smenu_x_t_) * std::min(1.0f, dt * 14.0f);
                }
                bool slotHoverActive = false;       // ease each slot's "+"/"Add Effect" hover in/out
                for (int mm = 1; mm <= 6; ++mm) {
                    const float target = (slot_title_hover_ == mm) ? 1.0f : 0.0f;
                    slot_hover_t_[mm] += (target - slot_hover_t_[mm]) * std::min(1.0f, dt * 44.0f);
                    if (std::abs(target - slot_hover_t_[mm]) > 0.01f) slotHoverActive = true;
                }
                const bool alive = reorder_active_ || fx_menu_module_ >= 0 || wiggle_module_ >= 0
                                 || slot_title_hover_ >= 0 || wiggle_fade_m_ >= 0 || swap_anim_m_ >= 0
                                 || slot_menu_m_ >= 0 || slotHoverActive || land_glow_t_ > 0.0f;
                if (!alive) { anim_sub_ = -1; return false; }
                request_repaint();   // keep frames flowing for the animations
                return true;
            });
    }

    // ── Reorder ("lift a piece of the seamless screen") ──────────────────────
    // The band is one seamless surface, so reorder = lift the grabbed module's
    // slice (icon + controls follow the cursor on a shadowed tile), leave a recess
    // where it was, and slide neighbours over. Split into: (1) reorder_update —
    // advance the tweens; (2) reorder_under — recess + shadow + raised tile, drawn
    // BEFORE the module content so the content lands on top of the tile; (3) the
    // macro-knob pass (in paint, after the content) — cover + redraw the moving
    // knobs. The slot band itself needs no cover: the seamless bg IS the surface,
    // and content simply redraws at its offset (reorder_slot_dx / reorder_knob_dx).

    // Cover one module's macro-knob band (cream) so the baked ring/needle/"---"
    // can be redrawn at the reorder offset.
    void cover_knob(cv::Canvas& canvas, const PanelTransform& t, int i) const {
        const float kx = knobs_[(size_t)i].cx;
        canvas.set_fill_color(hex_color("#fbf4e6"));
        canvas.fill_rect(t.ox + (kx - 61.4f) * t.scale, t.oy + 452 * t.scale,
                         117.4f * t.scale, (636 - 452) * t.scale);
    }
    // Redraw one module's macro knob (baked ring + needle, NOT the "---" label,
    // which is drawn natively by draw_module_strip_labels) shifted by dxk, scaled s.
    void draw_module_knob(cv::Canvas& canvas, const PanelTransform& t, int i,
                          float dxk, float s, float alpha) const {
        const float kx = knobs_[(size_t)i].cx;
        char w[224];
        std::snprintf(w, sizeof(w),
            "<g transform=\"translate(%.2f 0) translate(%.2f %.2f) scale(%.4f) translate(%.2f %.2f)\" opacity=\"%.3f\">",
            dxk, kx, 544.0f, s, -kx, -544.0f, alpha);
        draw_mini(canvas, t, std::string(w) +
                  band_paths(kx - 61.4f, 452, kx + 56.0f, 600) +
                  needle_svg(knobs_[(size_t)i], i, 0) + "</g>");
    }

    // ── Commit a reorder: actually move the module data so the new order sticks ──
    int bypass_hit(int m) const {
        for (size_t i = 0; i < hits_.size(); ++i)
            if (hits_[i].toggle && hits_[i].knob_index == m) return (int)i;
        return -1;
    }
    void swap_modules(int a, int b) {
        std::swap(module_fx_[(size_t)a], module_fx_[(size_t)b]);
        std::swap(slot_vis_[a][0], slot_vis_[b][0]);
        std::swap(slot_vis_[a][1], slot_vis_[b][1]);
        for (int i = 0; i < 12; ++i) {
            std::swap(mod_val_[a][i], mod_val_[b][i]);
            std::swap(mod_dep_[a][i], mod_dep_[b][i]);
        }
        const float va = element_value(a), vb = element_value(b);   // macro-knob values
        set_element_value(a, vb); set_element_value(b, va);
        const int ba = bypass_hit(a), bb = bypass_hit(b);            // bypass on/off
        if (ba >= 0 && bb >= 0) std::swap(hits_[(size_t)ba].on, hits_[(size_t)bb].on);
    }
    // Rotate modules so `src` lands at `dt` (adjacent swaps shift the run over).
    void commit_reorder(int src, int dt) {
        if (src < 1 || src > 6 || dt < 1 || dt > 6 || src == dt) return;
        const int step = (dt > src) ? 1 : -1;
        for (int m = src; m != dt; m += step) swap_modules(m, m + step);
    }

    // Advance the reorder tweens. Returns true while still animating; finalizes
    // (commits the new order + clears state) when the drop tween completes.
    bool reorder_update() {
        const int src = reorder_cand_, dt = drop_target_;
        const float fdt = frame_clock() ? frame_clock()->dt() : 1.0f / 60.0f;
        bool animating = false;
        auto ease = [&](float& v, float target, float eps) {
            const float d = target - v;
            if (std::abs(d) > eps) { v += d * 0.30f; animating = true; } else v = target;
        };
        for (int p = 1; p <= 6; ++p) {
            float target = 0.0f;
            if (p != src) {
                if (src < dt && p > src && p <= dt) target = -1.0f;
                else if (src > dt && p >= dt && p < src) target = 1.0f;
            }
            ease(slide_[p], target, 0.01f);
        }
        if (dropping_) {
            drop_t_ = std::min(1.0f, drop_t_ + fdt / DROP_DUR);
            const float e = ease_in_out(drop_t_);
            ghost_dxs_ = drop_dxs0_ + (drop_dxs1_ - drop_dxs0_) * e;
            ghost_dxk_ = drop_dxk0_ + (drop_dxk1_ - drop_dxk0_) * e;
            // Hold the full lift (scale + tile) through the slide so the background
            // doesn't change colour while the module returns — the fade only begins
            // once it lands (finalize → land_glow).
            if (drop_t_ < 1.0f) animating = true;
            else {                          // settled → COMMIT the new order, then rest
                settle_lo_ = std::min(reorder_cand_, drop_target_);   // affected macro knobs
                settle_hi_ = std::max(reorder_cand_, drop_target_);
                settle_frames_ = 3;         // hide the base's one-frame stale-value render
                land_glow_m_ = drop_target_; land_glow_t_ = 1.0f;     // bg eases from lift colour → normal
                commit_reorder(reorder_cand_, drop_target_);
                reorder_active_ = false; dropping_ = false; reorder_cand_ = -1;
                for (int p = 0; p < 8; ++p) slide_[p] = 0.0f;
                ghost_dxs_ = ghost_dxk_ = 0; scale_ = 1.0f; request_repaint();
                return false;
            }
        } else {
            ease(scale_, REORDER_SCALE, 0.002f);   // "lift toward the user" on grab
        }
        return animating;
    }

    // Drawn BEFORE the module content: the recess left behind + the raised tile
    // (with a soft drop shadow) the grabbed module rides on.
    void reorder_under(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float v) { return t.ox + v * t.scale; };
        auto Y = [&](float v) { return t.oy + v * t.scale; };
        auto S = [&](float v) { return v * t.scale; };
        const int src = reorder_cand_;
        const float w0 = SLOT_X1[src] - SLOT_X0[src], h0 = 442.0f - 154.0f, cyB = 298.0f;
        const float liftP = std::clamp((scale_ - 1.0f) / (REORDER_SCALE - 1.0f), 0.0f, 1.0f);
        // (No recess — the band keeps its flat colour where the module lifts off.)
        // Raised tile at the dragged position.
        const float cx = slot_center(src) + ghost_dxs_, s = scale_;
        const float w = w0 * s, h = h0 * s, x = cx - w * 0.5f, y = cyB - h * 0.5f;
        for (int i = 4; i >= 1; --i) {   // soft shadow: stacked translucent rects, offset down
            const float sp = (float)i * 4.0f;
            g.set_fill_color(hex_color("#2e271c").with_alpha(0.05f * liftP));
            g.fill_rounded_rect(X(x - sp * 0.3f), Y(y + sp), S(w + sp * 0.6f), S(h), S(18));
        }
        g.set_fill_color(hex_color("#efe9de"));   // tile: a touch lighter than the bg → reads raised
        g.fill_rounded_rect(X(x), Y(y), S(w), S(h), S(16));
    }

    // ── FX-type picker (icon grid laid OVER the module strip, JUCE-style) ───────
    static constexpr int FX_COLS = 6, FX_ROWS = 2;
    static constexpr float FX_CW = 178, FX_CH = 130;
    // Controls view: knob Ø50 (JUCE SaturnRingKnob) scaled to this band. The
    // control sits where the icon sat (cell-top + 54) with the param label at the
    // same cell-bottom (+98) row, so picker and controls share one label rhythm.
    static constexpr float CTL_KNOB_D = 64.0f;
    static constexpr float CTL_PI = 3.14159265f;
    // JUCE sizes its control fonts with FontOptions().withHeight(s) (total font
    // HEIGHT = s); Pulp's set_font takes an em/point size, which renders ~1.25×
    // larger for the same number. Cap height ≈ 0.8·em, so scale JUCE font values
    // by this to match on screen. (The preset browser fonts were tuned visually
    // and already absorb this, so it's applied only to the ported control fonts.)
    static constexpr float CTL_FONT_K = 0.8f;

    // Covers the module area (slot panels + macro knobs) so it reads as a
    // continuation of the editor, not a floating modal.
    // The slot-panel strip ONLY — the darker #e8e1d5 section directly below the
    // preset bar and above the macro knobs. Matches the strip's OUTER silhouette
    // exactly: left x=91 (LFO panel left edge), right x=1209 (MASTER panel right),
    // top y=156, bottom y=439.4, with 49.4px outer corner radius (the big rounding
    // on the LFO/MASTER ends). The overlay covers this footprint perfectly so it
    // reads as a continuation of that darker section, NOT the full module strip.
    Rect4 fx_panel() const { return {91.0f, 156.0f, 1209.0f, 439.4f}; }
    static constexpr float FX_PANEL_RADIUS = 49.4f;
    void fx_grid_origin(float& gx0, float& gy0) const {
        const Rect4 p = fx_panel();
        gx0 = p.x0 + ((p.x1 - p.x0) - FX_COLS * FX_CW) * 0.5f;
        gy0 = p.y0 + ((p.y1 - p.y0) - FX_ROWS * FX_CH) * 0.5f;
    }
    int fx_cell_at(float px, float py) const {
        float gx0, gy0; fx_grid_origin(gx0, gy0);
        for (int i = 0; i < (int)fx_types_.size(); ++i) {
            const float cx = gx0 + (i % FX_COLS) * FX_CW, cy = gy0 + (i / FX_COLS) * FX_CH;
            if (px >= cx && px <= cx + FX_CW && py >= cy && py <= cy + FX_CH) return i;
        }
        return -1;
    }
    Rect4 fx_close_rect() const { const Rect4 p = fx_panel(); return {p.x0 + 22, p.y0 + 22, p.x0 + 52, p.y0 + 52}; }

    void fx_animate() {   // fade in/out; finalize close when invisible
        const float target = fx_closing_ ? 0.0f : 1.0f;
        const float d = target - fx_alpha_;
        if (std::abs(d) > 0.01f) { fx_alpha_ += d * 0.35f; request_repaint(); }
        else { fx_alpha_ = target; if (fx_closing_) { fx_closing_ = false; fx_menu_module_ = -1; } }
        // Per-cell hover progress eases toward 1 (hovered) / 0 (not) so the icon
        // grows/shrinks smoothly. Only the hovered cell rises while closing pulls all back.
        const float step = (frame_clock() ? frame_clock()->dt() : 1.0f / 60.0f) / FX_HOVER_DUR;
        for (int i = 0; i < 12; ++i) {
            const float tgt = (!fx_closing_ && fx_hover_ == i) ? 1.0f : 0.0f;
            if (fx_cell_t_[i] < tgt) fx_cell_t_[i] = std::min(tgt, fx_cell_t_[i] + step);
            else if (fx_cell_t_[i] > tgt) fx_cell_t_[i] = std::max(tgt, fx_cell_t_[i] - step);
        }
        auto ease = [&](float& v, bool on) { const float tg = (!fx_closing_ && on) ? 1.0f : 0.0f;
            if (v < tg) v = std::min(tg, v + step); else if (v > tg) v = std::max(tg, v - step); };
        ease(close_t_, fx_hover_ == -2);     // close-button hover grow + text
        ease(remove_t_, fx_hover_ == -3);    // remove-button hover
    }
    static float smoothstep(float x) { return x * x * (3.0f - 2.0f * x); }   // ease in/out

    // The colour for cell i's icon AND label — kept in one place so they never
    // drift apart. Selected → white; otherwise the label brown lightened toward
    // white by the SAME HOVER_LIGHTEN every other control uses, scaled by this
    // cell's eased hover progress (so the lighten eases in/out with the grow).
    std::string fx_cell_color(int i, bool sel) const {
        if (sel) return "#ffffff";
        return lighten_hex(CONTROL_LABEL, HOVER_LIGHTEN * smoothstep(fx_cell_t_[(size_t)i]));
    }
    void fx_close() { fx_closing_ = true; ensure_animating(); request_repaint(); }

    // The module band drawn as ONE seamless surface (matching the advanced-view
    // panel bg): a single #e8e1d5 rounded rect that covers the baked per-module
    // cards and the cream gaps between them. Module content (titles, icons,
    // controls) is then drawn on top. (49.4 corner radius = the strip's outer
    // LFO/MASTER rounding, same as the advanced overlay.)
    void draw_band_bg(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float v) { return t.ox + v * t.scale; };
        auto Y = [&](float v) { return t.oy + v * t.scale; };
        auto S = [&](float v) { return v * t.scale; };
        g.set_fill_color(hex_color("#e8e1d5"));
        g.fill_rounded_rect(X(91.0f), Y(154.0f), S(1209.0f - 91.0f), S(442.0f - 154.0f), S(FX_PANEL_RADIUS));
    }

    // Thin vertical dividers between the 8 sections — 2/3 of the band height,
    // centred on the band's vertical axis. A subtle darker line on the seamless bg.
    void draw_dividers(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float v) { return t.ox + v * t.scale; };
        auto Y = [&](float v) { return t.oy + v * t.scale; };
        auto S = [&](float v) { return v * t.scale; };
        const float bandH = 442.0f - 154.0f, cyB = 298.0f;
        const float divH = bandH * (2.0f / 3.0f), y0 = cyB - divH * 0.5f;   // centred
        const float wpx = 2.0f;
        g.set_fill_color(hex_color("#d4cab9"));
        for (int i = 0; i < 7; ++i) {   // gap midpoints between adjacent slots
            const float dx = (SLOT_X1[i] + SLOT_X0[i + 1]) * 0.5f;
            g.fill_rect(X(dx) - S(wpx) * 0.5f, Y(y0), S(wpx), S(divH));
        }
    }

    // Bypassed modules: the icon + slot controls + bottom name now live on the
    // seamless band (drawn over the old bypass-fade rects), so dim them here too —
    // fade the slot toward the band colour and the name toward the panel.
    void draw_bypass_dim(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float v) { return t.ox + v * t.scale; };
        auto Y = [&](float v) { return t.oy + v * t.scale; };
        auto S = [&](float v) { return v * t.scale; };
        for (const auto& h : hits_) {
            if (!h.toggle || h.on) continue;                 // only bypassed (dot OFF)
            const int m = h.knob_index;
            if (m < 1 || m > 6 || module_fx_[(size_t)m].empty()) continue;
            const float scx = slot_center(m) + reorder_slot_dx(m), w = SLOT_X1[m] - SLOT_X0[m];
            g.set_fill_color(hex_color("#e8e1d5").with_alpha(BYPASS_DIM));
            g.fill_rect(X(scx - w * 0.5f), Y(154.0f), S(w), S(442.0f - 154.0f));   // icon + controls
            const float lcx = knobs_[(size_t)m].cx + reorder_knob_dx(m);
            g.set_fill_color(hex_color(PANEL_BG).with_alpha(BYPASS_DIM));
            g.fill_rect(X(lcx - 30.0f), Y(600.0f), S(60.0f), S(32.0f));            // bottom name
        }
    }

    // Slot title (top of each module): "LFO"/"MASTER" for the ends, the effect's
    // ICON (→ short NAME on hover) for loaded slots, "---" for empty ones. (The
    // baked titles are covered by the seamless band bg, so they're redrawn here.)
    // sel: 0 = all modules; >0 = only that module; <0 = all EXCEPT module -sel.
    // (Used during reorder to draw the grabbed module last/on-top and opaque.)
    void draw_module_names(cv::Canvas& g, const PanelTransform& t, int sel = 0) const {
        auto inc = [&](int m) { return sel == 0 || (sel > 0 && m == sel) || (sel < 0 && m != -sel); };
        auto title = [&](int i, const std::string& txt) {
            if (!inc(i)) return;
            const float cx = slot_center(i) + reorder_slot_dx(i);
            g.set_font(DD_BOLD, 16.0f * t.scale);
            g.set_fill_color(hex_color("#a19b92"));
            const float w = g.measure_text(txt);
            bt(g, txt, t.ox + cx * t.scale - w * 0.5f, t.oy + 192.0f * t.scale, t.scale);
        };
        title(0, "LFO");
        title(7, "MASTER");
        for (int i = 1; i <= 6; ++i) {
            if (!inc(i)) continue;
            const float cx = slot_center(i) + reorder_slot_dx(i);
            if (module_fx_[(size_t)i].empty()) {
                const bool hov = (slot_title_hover_ == i);
                g.set_font(DD_BOLD, 16.0f * t.scale);   // title swaps instantly (not animated)
                g.set_fill_color(hex_color("#a19b92"));
                const char* ttl = hov ? "Add Effect" : "---";
                const float w = g.measure_text(ttl);
                bt(g, ttl, t.ox + cx * t.scale - w * 0.5f, t.oy + 192.0f * t.scale, t.scale);
                const float th = slot_hover_t_[i];      // only the "+" eases in/out
                if (th > 0.01f) {
                    const float a = 17.0f, cy = 298.0f;
                    g.set_stroke_color(hex_color("#a19b92").with_alpha(th)); g.set_line_width(3.2f * t.scale);
                    g.set_line_cap(cv::LineCap::round);
                    g.begin_path(); g.move_to(t.ox + (cx - a) * t.scale, t.oy + cy * t.scale);
                    g.line_to(t.ox + (cx + a) * t.scale, t.oy + cy * t.scale); g.stroke_current_path();
                    g.begin_path(); g.move_to(t.ox + cx * t.scale, t.oy + (cy - a) * t.scale);
                    g.line_to(t.ox + cx * t.scale, t.oy + (cy + a) * t.scale); g.stroke_current_path();
                }
                continue;
            }
            if (slot_title_hover_ == i || wiggle_module_ == i) {   // hover preview OR active edit mode
                g.set_font(DD_BOLD, 16.0f * t.scale);              // same size as the LFO/MASTER titles
                g.set_fill_color(hex_color("#a19b92"));
                const int dots = (int)(anim_clock_ * 2.6f) % 4;        // EDIT → EDIT . .. ... → loop
                const std::string txt = std::string("EDIT") + (dots > 0 ? " " + std::string((size_t)dots, '.') : "");
                const float wmax = g.measure_text("EDIT ...");         // left-anchor so "EDIT" stays put
                bt(g, txt, t.ox + cx * t.scale - wmax * 0.5f, t.oy + 192.0f * t.scale, t.scale);
            } else {                                   // effect icon (centred in the title zone)
                const std::string ic = effect_icon(module_fx_[(size_t)i]);
                if (!ic.empty()) {
                    // Filter's icon fills its viewBox more than the others, so it
                    // reads larger — trim it to match the Delay clock's apparent size.
                    const float iscale = (module_fx_[(size_t)i] == "Filter") ? 0.85f : 1.0f;
                    const float sz = 52.9f * iscale;   // +15% over the original 46
                    g.draw_svg(ic, t.ox + (cx - sz * 0.5f) * t.scale, t.oy + (188.0f - sz * 0.5f) * t.scale,
                               sz * t.scale, sz * t.scale);   // icon stays put (only controls wiggle)
                }
            }
        }
    }

    static std::string upper(std::string s) {
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }

    // The icon SVG for an effect (from the picker's fx_types_), or "" if none.
    std::string effect_icon(const std::string& label) const {
        for (const auto& f : fx_types_) if (f.label == label) return f.icon;
        return "";
    }

    // The short module-strip name for an effect (JUCE getShortEffectName).
    static std::string fx_short(const std::string& label) {
        static const std::map<std::string, std::string> m = {
            {"Compressor", "COMP"}, {"EQ", "EQ"}, {"Filter", "FILTER"}, {"Distortion", "DIST"},
            {"Bit Crusher", "CRUSH"}, {"Ring Mod", "RING"}, {"Modulation", "MOD"}, {"Width", "WIDTH"},
            {"Delay", "DELAY"}, {"Lo-Fi", "LOFI"}, {"Reverb", "REVERB"}, {"Space", "SPACE"}};
        auto it = m.find(label);
        return it != m.end() ? it->second : upper(label);
    }

    // Bottom module-strip labels (under the macro knobs): the baked "---" is
    // replaced by the loaded effect's NAME, and the module whose advanced page is
    // open gets an accent pill (CONTROL_ACCENT) with white text — matching JUCE.
    void draw_module_strip_labels(cv::Canvas& g, const PanelTransform& t, int sel = 0) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        auto inc = [&](int m) { return sel == 0 || (sel > 0 && m == sel) || (sel < 0 && m != -sel); };
        for (int m = 1; m <= 6; ++m) {
            if (!inc(m)) continue;
            const bool empty = module_fx_[(size_t)m].empty();
            // An empty slot whose picker is open gets the accent pill too, so the user
            // sees which slot they're adding into.
            const bool pickingEmpty = empty && fx_menu_module_ == m && fx_view_ == 0 && !fx_closing_;
            if (empty && !pickingEmpty) continue;
            const float cx = knobs_[(size_t)m].cx + reorder_knob_dx(m);
            const std::string name = empty ? "---" : fx_short(module_fx_[(size_t)m]);
            g.set_fill_color(hex_color(PANEL_BG));          // cover the baked "---"
            g.fill_rect(X(cx - 36.0f), Y(602.0f), S(72.0f), S(26.0f));
            g.set_font(DD_BOLD, S(15.0f));
            const float ks = S(2.0f);                       // letter spacing (slightly wider kerning)
            float sw = 0.0f;                                // total spaced width
            for (char c : name) sw += g.measure_text(std::string(1, c)) + ks;
            sw = std::max(0.0f, sw - ks);
            const bool open = (fx_menu_module_ == m && !fx_closing_);
            const float cyc = Y(614.0f);                    // pill / text vertical centre
            if (open) {                                     // accent pill, white text
                const float ph = S(28.0f), pw = sw + S(26.0f);
                g.set_fill_color(hex_color(CONTROL_ACCENT));
                g.fill_rounded_rect(X(cx) - pw * 0.5f, cyc - ph * 0.5f, pw, ph, ph * 0.5f);
            }
            g.set_fill_color(hex_color(open ? "#ffffff" : CONTROL_LABEL));
            float dx = X(cx) - sw * 0.5f;                   // draw letter-spaced, centred on the pill
            const float baseline = cyc + S(5.2f);           // cap-height centre → vertically centred
            for (char c : name) {
                const std::string ch(1, c);
                bt(g, ch, dx, baseline, t.scale);
                dx += g.measure_text(ch) + ks;
            }
        }
    }

    // The two simple controls each effect slot shows in the module (closed) view —
    // a port of JUCE ModuleSlot pos1/pos2. Controls reflect the module's own live
    // values and are covered when its advanced page is open.
    // Draw one slot control (knob/toggle/radio) + its label at (ccx,ccy)/(lcx,lcy),
    // optionally with hover state and the edit-mode "−" badge.
    void draw_slot_pos(cv::Canvas& g, const PanelTransform& t, int m, int pos,
                       float ccx, float ccy, float lcx, float lcy, bool sh, bool badge) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        const float k = fx_k();
        const auto& cs = mod_ctls(m);
        const int ci = slot_vis_[m][pos];
        if (ci < 0 || ci >= (int)cs.size()) return;
        const auto& c = cs[(size_t)ci];
        const float macro = (float)element_value(m);
        if (c.kind == FxControl::Knob) {                 // Ø42 (JUCE SaturnRingKnobConfig::diameter)
            const bool valHov = sh && slot_hover_ring_ != 1, ringHov = sh && slot_hover_ring_ == 1;
            draw_ctl_knob(g, t, ccx, ccy, 42.0f * k, mod_val_[m][ci], mod_dep_[m][ci], macro, valHov, ringHov);
        } else if (c.kind == FxControl::Toggle) {
            draw_ctl_toggle(g, t, ccx, ccy, 20.0f * k * CTL_FONT_K, c.options[(size_t)mod_val_[m][ci]], sh);
        } else {
            draw_ctl_radio(g, t, ccx, ccy, k, c.options, (int)mod_val_[m][ci], -1, 10.5f, 14.5f);
        }
        g.set_font("Inter", S(14.0f * k * CTL_FONT_K));   // advanced-page label font
        g.set_fill_color(hex_color(CONTROL_LABEL));
        const float w = g.measure_text(c.label);
        g.fill_text(c.label, X(lcx) - w * 0.5f, Y(lcy));
        if (badge) {                                      // edit-mode remove badge (upper-left)
            const float outerR = (c.kind == FxControl::Knob) ? 29.4f * k : 30.0f * k;
            draw_remove_badge(g, t, ccx, ccy, outerR);
        }
    }

    void draw_slot_controls(cv::Canvas& g, const PanelTransform& t, int sel = 0) const {
        auto inc = [&](int m) { return sel == 0 || (sel > 0 && m == sel) || (sel < 0 && m != -sel); };
        float ctlY[2], labY[2]; slot_rows(ctlY, labY);   // advanced-page geometry
        for (int m = 1; m <= 6; ++m) {
            if (!inc(m) || module_fx_[(size_t)m].empty() || m == swap_anim_m_) continue;   // swap drawn below
            const float pcx = slot_center(m) + reorder_slot_dx(m);
            for (int pos = 0; pos < 2; ++pos) {
                if (slot_vis_[m][pos] < 0) continue;
                const bool sh = (slot_hover_m_ == m && slot_hover_pos_ == pos);
                float ccx = pcx, ccy = ctlY[pos]; wiggle_xy(m, ccx, ccy);   // edit-mode wiggle
                float lcx = pcx, lcy = labY[pos]; wiggle_xy(m, lcx, lcy);
                draw_slot_pos(g, t, m, pos, ccx, ccy, lcx, lcy, sh, false);
            }
        }
        if (swap_anim_m_ >= 0 && inc(swap_anim_m_) && !module_fx_[(size_t)swap_anim_m_].empty())
            draw_swap_anim(g, t, ctlY, labY);
    }

    // "Swap Parameters" carousel: the two controls rotate up one slot — the top one
    // slides off the top and reappears from the bottom — clipped to the slot band.
    void draw_swap_anim(cv::Canvas& g, const PanelTransform& t, const float ctlY[2], const float labY[2]) const {
        const int m = swap_anim_m_;
        const float pcx = slot_center(m);
        const float e = ease_in_out(std::clamp(swap_anim_t_, 0.0f, 1.0f));
        const float Sc = ctlY[1] - ctlY[0], Sl = labY[1] - labY[0];
        g.save();
        g.clip_rect(t.ox + SLOT_X0[m] * t.scale, t.oy + 216.0f * t.scale,
                    (SLOT_X1[m] - SLOT_X0[m]) * t.scale, (430.0f - 216.0f) * t.scale);
        // pos0 (new top) rises from the bottom slot to the top slot.
        draw_slot_pos(g, t, m, 0, pcx, ctlY[1] - e * Sc, pcx, labY[1] - e * Sl, false, false);
        // pos1 (old top → new bottom): main copy rises off the top; the wrapped copy
        // (one full loop = +2 slots) descends into the bottom slot. Clip hides the rest.
        const float mc = ctlY[0] - e * Sc, ml = labY[0] - e * Sl;
        draw_slot_pos(g, t, m, 1, pcx, mc, pcx, ml, false, false);
        draw_slot_pos(g, t, m, 1, pcx, mc + 2.0f * Sc, pcx, ml + 2.0f * Sl, false, false);
        g.restore();
    }

    // The slot view's two control rows + label rows, in SVG — identical to the
    // advanced page (ctl_cell): grid centred in the band, row offsets row_ctl_y/lab.
    void slot_rows(float ctlY[2], float labY[2]) const {
        const float gy = ((fx_panel().y1 - fx_panel().y0) / fx_k() - 2.0f * 95.0f) * 0.5f;
        // Module view rhythm: the slot ICON (centred at jy(183) ≈ gy+7.8) and the two
        // control rows are EQUALLY spaced (≈71 apart), the control→label gap is uniform
        // (≈33), and the bottom row's label sits at gy+182.5 — the SAME baseline as the
        // advanced page's bottom row (ctl_cell row 2 label). LFO/MASTER baked controls
        // are covered + redrawn (draw_lfo_master_controls) to match.
        // Both control rows moved UP ≈14px (≈10.6 local) with labels held fixed, so
        // every control gets the radio group's generous control→label padding (≈43.6
        // gap, matching the advanced page). Bottom label stays advanced-aligned.
        ctlY[0] = jy(gy + 66.6f);  labY[0] = jy(gy + 106.425f);
        ctlY[1] = jy(gy + 142.7f); labY[1] = jy(gy + 182.5f);
    }

    // MASTER limiter gain-reduction meter: a vertical column of 6 circles (Ø8,
    // gap 3), idle #A19B92@0.5 — JUCE EffectModuleRackModuleSlot MASTER GR draw.
    void draw_master_gr(cv::Canvas& g, const PanelTransform& t, float cx, float cyCenter) const {
        auto X = [&](float v) { return t.ox + v * t.scale; };
        auto Y = [&](float v) { return t.oy + v * t.scale; };
        auto S = [&](float v) { return v * t.scale; };
        const float k = fx_k();
        const int n = 6;
        const float d = 8.0f * k, pitch = (8.0f + 3.0f) * k;
        const float totalH = (float)n * d + (float)(n - 1) * 3.0f * k;
        const float y0 = cyCenter - totalH * 0.5f;
        g.set_fill_color(hex_color(CONTROL_LABEL).with_alpha(0.5f));   // idle (no audio)
        for (int i = 0; i < n; ++i)
            g.fill_circle(X(cx), Y(y0 + (float)i * pitch + d * 0.5f), S(d * 0.5f));
    }

    // The LFO and MASTER slots are baked into the captured SVG (radio + Sync
    // toggle / GR meter + Threshold knob), at the OLD looser spacing. Cover them
    // and re-draw natively at the (tighter) module-slot rows so all 8 modules
    // align. Labels are handled by draw_lfo_master_labels (drawn on top after).
    void draw_lfo_master_controls(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float v) { return t.ox + v * t.scale; };
        auto Y = [&](float v) { return t.oy + v * t.scale; };
        auto S = [&](float v) { return v * t.scale; };
        const float k = fx_k();
        float ctlY[2], labY[2]; slot_rows(ctlY, labY); (void)labY;
        auto cover = [&](int i) {                       // wipe the baked control area
            // Narrow (centred ±46) and stop at y=424 so the rect never reaches the
            // cards' large rounded OUTER corners (LFO bottom-left / MASTER bottom-right);
            // a full-width rect there left a hard square corner.
            g.set_fill_color(hex_color("#e8e1d5"));
            g.fill_rect(X(slot_center(i) - 46.0f), Y(200.0f), S(92.0f), S(224.0f));
        };
        cover(0); cover(7);
        // LFO (slot 0): Wave radio (pos1) + Sync toggle (pos2).
        const int wave = radio_.ok ? std::max(0, radio_.sel) : 0;
        // Module view: compact dots (Ø10.5 sp14.5) — split between the old LFO Ø9/12 and advanced Ø12/17.
        draw_ctl_radio(g, t, slot_center(0), ctlY[0], k, {"Sine", "Square", "Saw"}, wave, -1, 10.5f, 14.5f);
        draw_ctl_toggle(g, t, slot_center(0), ctlY[1], 20.0f * k * CTL_FONT_K, "Sync", false);
        // MASTER (slot 7): GR meter (pos1) + Threshold knob (pos2, rests at 0).
        draw_master_gr(g, t, slot_center(7), ctlY[0]);
        draw_ctl_knob(g, t, slot_center(7), ctlY[1], 42.0f * k, 0.0f, 0.0f, 0.0f, false, false);
    }

    // The baked LFO/MASTER slot param-labels are bold in the captured SVG. Cover
    // them and re-draw in the SAME regular font the effect slots use, so every
    // module's labels match. (Their control text stays as baked.)
    void draw_lfo_master_labels(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float v) { return t.ox + v * t.scale; };
        auto Y = [&](float v) { return t.oy + v * t.scale; };
        auto S = [&](float v) { return v * t.scale; };
        const float fs = S(14.0f * fx_k() * CTL_FONT_K);   // identical to draw_slot_controls
        // The baked labels are already wiped by draw_lfo_master_controls' big cover,
        // so we just DRAW the text here (no per-label cover rect — those clipped the
        // radio circles, which sit just above the label).
        auto lbl = [&](const char* txt, float pcx, float base) {
            g.set_font("Inter", fs);
            g.set_fill_color(hex_color(CONTROL_LABEL));
            const float w = g.measure_text(txt);
            g.fill_text(txt, X(pcx) - w * 0.5f, Y(base));
        };
        const float lc = slot_center(0), mc = slot_center(7);
        float ctlY[2], labY[2]; slot_rows(ctlY, labY); (void)ctlY;   // same rows as the effect slots
        lbl("Wave", lc, labY[0]);          // aligned with the other pos1 labels (controls moved up)
        lbl("Sync", lc, labY[1]);
        lbl("GR", mc, labY[0] + 6.0f);     // slightly lower: the GR meter is taller than a knob
        lbl("Threshold", mc, labY[1]);
        // (LFO Wave-radio option labels Sine/Square/Saw are drawn by draw_ctl_radio
        //  in draw_lfo_master_controls, at the new tighter row — no redraw here.)
    }

    // A slot control under (px,py): sets module/pos and `ring` (knob 0=value/1=ring,
    // radio = option index). Geometry mirrors draw_slot_controls.
    bool slot_ctl_at(float px, float py, int& om, int& opos, int& oring) const {
        oring = 0;
        const float k = fx_k();
        float ctlY[2], labY[2]; slot_rows(ctlY, labY);   // must match draw_slot_controls
        for (int m = 1; m <= 6; ++m) {
            if (module_fx_[(size_t)m].empty()) continue;
            const auto& cs = mod_ctls(m);
            const float pcx = slot_center(m);
            for (int pos = 0; pos < 2; ++pos) {
                const int ci = slot_vis_[m][pos];
                if (ci < 0 || ci >= (int)cs.size()) continue;
                const auto& c = cs[(size_t)ci];
                const float cy = ctlY[pos], dx = px - pcx, dy = py - cy;
                if (c.kind == FxControl::Knob) {
                    const float dist = std::sqrt(dx * dx + dy * dy);   // Ø42 knob: radii scaled 42/50
                    if (dist <= 16.8f * k) { om = m; opos = pos; oring = 0; return true; }
                    if (dist <= 37.0f * k && std::abs(std::atan2(dx, -dy)) < 0.75f * CTL_PI) {
                        om = m; opos = pos; oring = 1; return true;
                    }
                } else if (c.kind == FxControl::Toggle) {
                    if (std::abs(dx) < 50.0f * k && std::abs(dy) < 22.0f * k) {
                        om = m; opos = pos; oring = 0; return true;
                    }
                } else {   // radio: option rows
                    const float sp = 17.0f * k; const int n = (int)c.options.size();
                    const float startY = cy - (float)(n - 1) * sp * 0.5f;
                    for (int o = 0; o < n; ++o)
                        if (std::abs(py - (startY + (float)o * sp)) < sp * 0.5f && std::abs(dx) < 55.0f) {
                            om = m; opos = pos; oring = o; return true;
                        }
                }
            }
        }
        return false;
    }

    // ── Slot param-picker popup (customize layout) ───────────────────────────
    // Rows: itemId −2 = Set as Default, −1 = Swap Parameters, 0..n−1 = pick that
    // param. Geometry is shared by draw + hit-test via slot_menu_each.
    static constexpr float SMENU_W = 188.0f, SMENU_HEADER = 26.0f, SMENU_RH = 27.0f, SMENU_SEP = 9.0f;
    static constexpr float SMENU_PAD = 12.0f;   // equal padding on all four sides
    int slot_menu_param_count() const {
        const auto& cs = mod_ctls(slot_menu_m_); int n = 0;
        for (const auto& c : cs) if (c.assignable()) ++n;
        return n;
    }
    // SVG centre of a slot control (the popup's anchor / pointer target).
    void slot_ctl_center(int m, int pos, float& x, float& y) const {
        float ctlY[2], labY[2]; slot_rows(ctlY, labY);
        x = slot_center(m) + reorder_slot_dx(m);
        y = ctlY[std::clamp(pos, 0, 1)];
    }
    // The popup sits BESIDE the anchored knob (so it never covers the controls):
    // to the right if it fits, otherwise to the left, with a GAP for the pointer.
    static constexpr float SMENU_GAP = 50.0f;   // clear the knob + Saturn ring so the pointer doesn't cover it
    void slot_menu_box(float& x, float& y, float& h) const {
        h = 2.0f * SMENU_PAD + SMENU_HEADER + 2.0f * SMENU_RH + SMENU_SEP + (float)slot_menu_param_count() * SMENU_RH;
        const float ax = slot_menu_x_, ay = slot_menu_y_;
        x = (ax + SMENU_GAP + SMENU_W <= 1295.0f) ? ax + SMENU_GAP : ax - SMENU_GAP - SMENU_W;
        if (x < 10.0f) x = 10.0f;
        if (x + SMENU_W > 1295.0f) x = 1295.0f - SMENU_W;
        y = ay - SMENU_PAD - SMENU_HEADER - SMENU_RH;     // anchor near the first param rows
        if (y + h > 686.0f) y = 686.0f - h;
        if (y < 10.0f) y = 10.0f;
    }
    // Pointer triangle on the menu edge facing the knob.
    void slot_menu_tri(float& tx, float& ty, bool& pointLeft) const {
        float x, y, h; slot_menu_box(x, y, h);
        pointLeft = (x >= slot_menu_x_);     // menu right of the knob → arrow on its left, pointing left
        tx = pointLeft ? x : x + SMENU_W;
        // Point at the hovered assign column's control (1 → pos1, 2 → pos2); the
        // connected grid (no gaps) keeps this stable, else the anchored control.
        const int tp = (slot_menu_hover_badge_ == 1) ? 0
                     : (slot_menu_hover_badge_ == 2) ? 1 : slot_menu_pos_;
        float tcx, tcy; slot_ctl_center(slot_menu_m_, tp, tcx, tcy);
        ty = std::clamp(tcy, y + 18.0f, y + h - 18.0f);
    }
    template <class F> void slot_menu_each(F&& f) const {
        float x, y, h; slot_menu_box(x, y, h);
        float cy = y + SMENU_PAD + SMENU_HEADER;
        f(-2, x, cy); cy += SMENU_RH;                 // Set as Default
        f(-1, x, cy); cy += SMENU_RH + SMENU_SEP;     // Swap Parameters (+ separator gap)
        const auto& cs = mod_ctls(slot_menu_m_);
        for (int i = 0; i < (int)cs.size(); ++i)      // assignable params only (skip GR meter)
            if (cs[i].assignable()) { f(i, x, cy); cy += SMENU_RH; }
    }
    // The connected "1"/"2" assignment grid (two columns, cells touching) on the
    // right of the param rows. cw = column width, gtop = first param-row top.
    static constexpr float SMENU_CW = 26.0f;
    void slot_menu_grid(float& gx, float& gtop, float& cw, float& ch, int& np) const {
        float x, y, h; slot_menu_box(x, y, h);
        cw = SMENU_CW; ch = SMENU_RH; np = slot_menu_param_count();
        gx = x + SMENU_W - 2.0f * cw - SMENU_PAD;
        gtop = y + SMENU_PAD + SMENU_HEADER + 2.0f * SMENU_RH + SMENU_SEP;
    }
    // Popup-relative font, matching the slot-control parameter labels (so the popup
    // text is at least as large). pf = label em-size in SVG units.
    float smenu_pf() const { return 14.0f * fx_k() * CTL_FONT_K; }
    // Header close-X centre (right of "Select Parameter").
    void slot_menu_x_btn(float& cx, float& cy) const {
        float x, y, h; slot_menu_box(x, y, h);
        cx = x + SMENU_W - SMENU_PAD - 5.0f; cy = y + SMENU_PAD + 10.0f;
    }

    // Hit: id (-99 outside / -98 chrome / -2 Set-Default / -1 Swap / 0..n param) and,
    // for a param row, the assign column under the cursor (1/2, else 0 = row body).
    void slot_menu_hit(float px, float py, int& id, int& badge) const {
        badge = 0; id = -99;
        float x, y, h; slot_menu_box(x, y, h);
        if (px < x || px > x + SMENU_W || py < y || py > y + h) return;
        id = -98;
        slot_menu_each([&](int rid, float rx, float ry) {
            if (py >= ry && py < ry + SMENU_RH) id = rid;
        });
        { float cxx, cyy; slot_menu_x_btn(cxx, cyy);          // header close-X
          if (std::abs(px - cxx) < 12.0f && std::abs(py - cyy) < 12.0f) { id = -3; badge = 0; return; } }
        if (id >= 0) {   // param row → which assign column (the cells touch, no gaps)
            float gx, gtop, cw, ch; int np; slot_menu_grid(gx, gtop, cw, ch, np);
            if (px >= gx && px < gx + 2.0f * cw && py >= gtop && py < gtop + (float)np * ch)
                badge = (px < gx + cw) ? 1 : 2;
        }
    }
    void draw_slot_menu(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float v) { return t.ox + v * t.scale; };
        auto Y = [&](float v) { return t.oy + v * t.scale; };
        auto S = [&](float v) { return v * t.scale; };
        float x, y, h; slot_menu_box(x, y, h);
        float tx, ty; bool pointLeft; slot_menu_tri(tx, ty, pointLeft);
        const float ts = 9.0f, dir = pointLeft ? -1.0f : 1.0f;       // pointer triangle apex direction
        const float pf = smenu_pf();                                 // label size = slot-control label size
        g.set_fill_color(hex_color(PANEL_BG));                       // panel
        g.fill_rounded_rect(X(x), Y(y), S(SMENU_W), S(h), S(8.0f));
        g.set_stroke_color(hex_color("#a19b92")); g.set_line_width(S(1.5f));   // same stroke as the preset popup
        g.stroke_rounded_rect(X(x), Y(y), S(SMENU_W), S(h), S(8.0f));
        // Pointer: fill bg over the edge + the apex, then stroke the two slanted edges.
        g.set_fill_color(hex_color(PANEL_BG));
        g.begin_path();
        g.move_to(X(tx), Y(ty - ts)); g.line_to(X(tx + dir * ts), Y(ty)); g.line_to(X(tx), Y(ty + ts));
        g.line_to(X(tx - dir * 2.0f), Y(ty + ts)); g.line_to(X(tx - dir * 2.0f), Y(ty - ts)); g.close_path();
        g.fill_current_path();
        g.set_stroke_color(hex_color("#a19b92")); g.set_line_width(S(1.5f));
        g.begin_path(); g.move_to(X(tx), Y(ty - ts)); g.line_to(X(tx + dir * ts), Y(ty)); g.line_to(X(tx), Y(ty + ts)); g.stroke_current_path();
        g.set_font(DD_BOLD, S(pf));                                  // header = MORE bold
        g.set_fill_color(hex_color(CONTROL_LABEL));
        bt(g, "Select Parameter", X(x + SMENU_PAD), Y(y + SMENU_PAD + 16.0f), t.scale);
        { float cxx, cyy; slot_menu_x_btn(cxx, cyy);                 // close-X (grows on hover)
          const float ss = smenu_x_t_ * smenu_x_t_ * (3.0f - 2.0f * smenu_x_t_);
          const float r = 5.0f * (1.0f + 0.45f * ss);
          g.set_stroke_color(hex_color(slot_menu_hover_ == -3 ? "#766f64" : "#a19b92"));
          g.set_line_width(S(1.8f)); g.set_line_cap(cv::LineCap::round);
          g.begin_path(); g.move_to(X(cxx - r), Y(cyy - r)); g.line_to(X(cxx + r), Y(cyy + r)); g.stroke_current_path();
          g.begin_path(); g.move_to(X(cxx + r), Y(cyy - r)); g.line_to(X(cxx - r), Y(cyy + r)); g.stroke_current_path(); }
        const auto& cs = mod_ctls(slot_menu_m_);
        const int p1 = slot_vis_[slot_menu_m_][0], p2 = slot_vis_[slot_menu_m_][1];
        slot_menu_each([&](int id, float rx, float ry) {             // row labels = LESS bold (regular)
            const std::string label = (id == -2) ? "Set as Default" : (id == -1) ? "Swap Parameters"
                                      : cs[(size_t)id].label;
            const bool chromeHov = (id < 0 && slot_menu_hover_ == id);   // Set Default / Swap hover
            if (chromeHov) {
                g.set_fill_color(hex_color(CONTROL_LABEL).with_alpha(0.18f));
                g.fill_rounded_rect(X(rx + 6.0f), Y(ry + 1.0f), S(SMENU_W - 12.0f), S(SMENU_RH - 2.0f), S(5.0f));
            }
            g.set_font("Inter", S(pf));
            g.set_fill_color(hex_color(chromeHov ? "#766f64" : CONTROL_LABEL));
            g.fill_text(label, X(rx + SMENU_PAD), Y(ry + 19.0f));
        });
        // Connected 1/2 grid: cell fills + numerals, then the brown grid lines on top.
        float gx, gtop, cw, ch; int np; slot_menu_grid(gx, gtop, cw, ch, np);
        slot_menu_each([&](int id, float rx, float ry) {
            if (id < 0) return;
            for (int col = 1; col <= 2; ++col) {
                const bool filled = (col == 1) ? (id == p1) : (id == p2);
                const bool hov = (slot_menu_hover_ == id && slot_menu_hover_badge_ == col);
                const float cx0 = gx + (float)(col - 1) * cw;
                if (filled) g.set_fill_color(hex_color(hov ? "#8b857c" : CONTROL_LABEL));
                else if (hov) g.set_fill_color(hex_color(CONTROL_LABEL).with_alpha(0.3f));
                else g.set_fill_color(hex_color(PANEL_BG));
                g.fill_rect(X(cx0), Y(ry), S(cw), S(ch));
                g.set_font("Inter", S(pf));
                g.set_fill_color(hex_color(filled ? "#e8e1d5" : CONTROL_LABEL));
                const char* num = (col == 1) ? "1" : "2";
                const float tw = g.measure_text(num);
                g.fill_text(num, X(cx0 + cw * 0.5f) - tw * 0.5f, Y(ry + ch * 0.5f + pf * 0.34f));
            }
        });
        g.set_fill_color(hex_color("#a19b92"));                      // grid lines (1.5, like the border)
        const float gw = 2.0f * cw, gh = (float)np * ch;
        for (int r = 0; r <= np; ++r) g.fill_rect(X(gx), Y(gtop + (float)r * ch) - S(0.75f), S(gw), S(1.5f));
        for (int c = 0; c <= 2; ++c) g.fill_rect(X(gx + (float)c * cw) - S(0.75f), Y(gtop), S(1.5f), S(gh));
    }

    // A badge / row click (JUCE EffectModuleRack ParamPicker logic). Badge → assign
    // that param to pos1/pos2 (clear if already there, displacing the other); row
    // body → cycle. Param edits keep the menu open; Swap / Set-Default dismiss it.
    void slot_menu_click(int id, int badge) {
        const int m = slot_menu_m_;
        if (m < 0) return;
        int& v1 = slot_vis_[m][0]; int& v2 = slot_vis_[m][1];
        if (id == -2) {                                  // Set as Default → first two assignable
            const auto& cs = mod_ctls(m); v1 = v2 = -1; int f = 0;
            for (int i = 0; i < (int)cs.size() && f < 2; ++i)
                if (cs[i].assignable()) { (f == 0 ? v1 : v2) = i; ++f; }
            slot_menu_m_ = -1;
        } else if (id == -1) {                          // Swap → animate the carousel (popup stays open)
            std::swap(v1, v2);
            swap_anim_m_ = m; swap_anim_t_ = 0.0f; ensure_animating();
        }
        else if (id >= 0) {
            if (badge == 1) {
                if (v1 == id) v1 = -1; else { if (v2 == id) v2 = -1; v1 = id; }
            } else if (badge == 2) {
                if (v2 == id) v2 = -1; else { if (v1 == id) v1 = -1; v2 = id; }
            } else {                                     // row body → cycle (onItemSelected)
                if (v2 == id) v2 = -1;
                else if (v1 == id) {
                    v2 = v1; v1 = -1;
                    const auto& cs = mod_ctls(m);
                    for (int i = 0; i < (int)cs.size(); ++i)
                        if (cs[i].assignable() && i != v2) { v1 = i; break; }
                } else v1 = id;
            }                                            // param edits keep the menu open
        }
        request_repaint();
    }

    // Close / remove button centres (above the input / output meters) + hit zones.
    float close_btn_cx() const { return jx(55.0f); }     // above input-meter bars
    float remove_btn_cx() const { return jx(809.0f); }   // above output-meter bars
    float btn_cy() const { return jy(29.0f); }           // kHeaderPadY 15 + btn 28/2
    bool in_close_btn(float px, float py) const {
        const float k = fx_k(); return std::abs(px - close_btn_cx()) < 18.0f * k && std::abs(py - btn_cy()) < 18.0f * k;
    }
    bool in_remove_btn(float px, float py) const {
        const float k = fx_k(); return std::abs(px - remove_btn_cx()) < 20.0f * k && std::abs(py - btn_cy()) < 20.0f * k;
    }
    // Remove the open module's effect and drop back to the icon browser.
    void remove_effect() {
        if (fx_menu_module_ >= 0) module_fx_[(size_t)fx_menu_module_].clear();
        fx_view_ = 0; fx_hover_ = -1; remove_t_ = 0;
        for (float& tt : fx_cell_t_) tt = 0.0f;
        request_repaint();
    }

    // Controls-view close (above input meter) + remove (above output meter): each
    // grows on hover and slides its label out ("Close Window" right / "Remove
    // Effect" left). The remove icon is JUCE's backspace-tag-with-X.
    void draw_fx_close_remove(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        const float k = fx_k(), cy = btn_cy();
        // Close ✕
        {
            const float cx = close_btn_cx(), r = 5.5f * k * (1.0f + 0.30f * smoothstep(close_t_));
            g.set_stroke_color(hex_color(close_t_ > 0.5f ? "#766f64" : "#a19b92"));
            g.set_line_width(S(3.0f * k)); g.set_line_cap(cv::LineCap::round);
            g.begin_path(); g.move_to(X(cx - r), Y(cy - r)); g.line_to(X(cx + r), Y(cy + r)); g.stroke_current_path();
            g.begin_path(); g.move_to(X(cx + r), Y(cy - r)); g.line_to(X(cx - r), Y(cy + r)); g.stroke_current_path();
        }
        // Remove (backspace-tag-with-X, JUCE EffectSelectorOverlay) — smaller, thinner.
        // Dimmed to 15% and non-interactive for the LFO (a pinned, non-removable slot).
        {
            const bool canRemove = (fx_menu_module_ != 0);
            const float cx = remove_btn_cx(), sz = 32.0f * k * (1.0f + (canRemove ? 0.30f * smoothstep(remove_t_) : 0.0f));
            const bool hov = canRemove && remove_t_ > 0.5f;
            // Non-removable (LFO) → JUCE dims the icon to 15%. The canvas has no
            // global alpha, so bake medBrown@0.15 over the panel bg (#e8e1d5).
            const std::string stroke = canRemove ? std::string("#a19b92") : std::string("#ddd7cb");
            const std::string oc = hov ? std::string("#a19b92") : std::string("none");
            const std::string xc = hov ? std::string("#ffffff") : stroke;
            const std::string svg =
                std::string("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">")
                + "<path d=\"M2.7716 13.5185 L7.43827 17.5185 C7.80075 17.8292 8.26243 18 8.73985 18 L18 18 "
                  "C19.1046 18 20 17.1046 20 16 L20 8 C20 6.89543 19.1046 6 18 6 L8.73985 6 "
                  "C8.26243 6 7.80075 6.17078 7.43827 6.48149 L2.7716 10.4815 "
                  "C1.84038 11.2797 1.84038 12.7203 2.7716 13.5185 Z\" fill=\"" + oc + "\" stroke=\"" + stroke + "\" stroke-width=\"1.5\"/>"
                + "<path d=\"M11 10 L15 14 M11 14 L15 10\" fill=\"none\" stroke=\"" + xc + "\" stroke-width=\"1.5\"/></svg>";
            g.draw_svg(svg, X(cx) - S(sz) * 0.5f, Y(cy) - S(sz) * 0.5f, S(sz), S(sz));
        }
    }

    void draw_fx_overlay(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        const float a = fx_alpha_;
        const Rect4 p = fx_panel();
        // Fill the slot-panel band with the same darker #e8e1d5 the slot panels
        // use, so the overlay reads as a continuation of that section (NO scrim —
        // everything outside the band stays fully transparent / click-to-close).
        // 49.4px corner radius matches the strip's outer (LFO/MASTER) rounding.
        g.set_fill_color(hex_color("#e8e1d5").with_alpha(a));
        g.fill_rounded_rect(X(p.x0), Y(p.y0), S(p.x1 - p.x0), S(p.y1 - p.y0), S(FX_PANEL_RADIUS));
        if (a < 0.25f) return;   // grid cross-fades in over the modules
        if (fx_view_ == 1) {   // Controls view: close above the input meter, remove above output
            draw_fx_controls(g, t);
            if (fx_menu_module_ == 0) {   // LFO: visualiser replaces the input fader, depth
                draw_lfo_visualizer(g, t); // sliders fill cols 4–6, and no in/out meters show
                draw_lfo_depth_sliders(g, t);
            } else {
                draw_fx_meters(g, t);
            }
            draw_fx_close_remove(g, t); return;
        }
        { const Rect4 c = fx_close_rect();                                   // picker close ✕ (top-left)
          const float xcx = (c.x0 + c.x1) * 0.5f, xcy = (c.y0 + c.y1) * 0.5f, r = 8;
          g.set_stroke_color(hex_color(fx_hover_ == -2 ? "#766f64" : "#a19b92"));
          g.set_line_width(S(3.0f)); g.set_line_cap(cv::LineCap::round);
          g.begin_path(); g.move_to(X(xcx - r), Y(xcy - r)); g.line_to(X(xcx + r), Y(xcy + r)); g.stroke_current_path();
          g.begin_path(); g.move_to(X(xcx + r), Y(xcy - r)); g.line_to(X(xcx - r), Y(xcy + r)); g.stroke_current_path(); }
        float gx0, gy0; fx_grid_origin(gx0, gy0);
        const std::string& cur = module_fx_[(size_t)fx_menu_module_];
        for (int i = 0; i < (int)fx_types_.size(); ++i) {
            const int col = i % FX_COLS, row = i / FX_COLS;
            const float cx = gx0 + col * FX_CW, cy = gy0 + row * FX_CH;
            const bool sel = cur == fx_types_[(size_t)i].label;
            if (sel) {   // selected effect: accent fill (hover no longer draws a bg)
                g.set_fill_color(hex_color("#7b6896").with_alpha(0.9f));
                g.fill_rounded_rect(X(cx + 12), Y(cy + 10), S(FX_CW - 24), S(FX_CH - 20), S(16));
            }
            // Icon + label share ONE hover colour (eased lighten) so they always
            // match; hover also grows the icon about its centre (same smoothstep).
            const std::string col_hex = fx_cell_color(i, sel);
            const float isz = 48.0f * (1.0f + (FX_HOVER_SCALE - 1.0f) * smoothstep(fx_cell_t_[i]));
            const float icx = cx + (FX_CW - isz) * 0.5f, icy = cy + 54.0f - isz * 0.5f;
            std::string ic = fx_types_[(size_t)i].icon;
            recolor_svg(ic, col_hex);
            if (!ic.empty()) g.draw_svg(ic, X(icx), Y(icy), S(isz), S(isz));
            g.set_font(DD_BOLD, S(19));
            g.set_fill_color(hex_color(col_hex));
            const float w = g.measure_text(fx_types_[(size_t)i].label);
            bt(g, fx_types_[(size_t)i].label, X(cx + FX_CW * 0.5f) - w * 0.5f,
               Y(cy + 98.0f), t.scale);
        }
    }

    // ── Effect Controls view ────────────────────────────────────────────────
    // The whole JUCE EffectControlPanel (850px-wide panel) is mapped into our
    // slot-strip band by ONE uniform factor k = band/850, origin at the band's
    // top-left. That tightens the 6×2 grid to JUCE's spacing (grid is 660 wide,
    // centred → 95px margins each side) and opens the side margins for the
    // input/output meters — exactly the JUCE proportions.
    float fx_k() const { const Rect4 p = fx_panel(); return (p.x1 - p.x0) / 850.0f; }
    float jx(float v) const { return fx_panel().x0 + v * fx_k(); }   // JUCE px → SVG x
    float jy(float v) const { return fx_panel().y0 + v * fx_k(); }   // JUCE px → SVG y

    void draw_fx_controls(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        const float k = fx_k();
        const float macro = module_macro();
        const int mm = fx_menu_module_;
        const auto& cs = mod_ctls(mm);
        for (int i = 0; i < (int)cs.size(); ++i) {
            const auto& c = cs[i];
            float cellJX, cellJY, ccx, knobY; ctl_cell(i, cellJX, cellJY, ccx, knobY);
            const float labY = jy(cellJY + row_lab_y(c.row));              // param label baseline
            const bool hov = ctl_hover_ == i;
            if (c.kind == FxControl::Meter) {   // GR meter (centred over its colSpan)
                draw_gr_meter(g, t, jx(cellJX + 55.0f * (float)c.colSpan), knobY, labY, c.label);
                continue;
            }
            if (c.kind == FxControl::Knob) {
                const bool valHov = (hov && ctl_hover_opt_ != 1) || (knob_drag_ == i && !knob_drag_ring_);
                const bool ringHov = (hov && ctl_hover_opt_ == 1) || (knob_drag_ == i && knob_drag_ring_);
                draw_ctl_knob(g, t, ccx, knobY, 50.0f * k, mod_val_[mm][i], mod_dep_[mm][i], macro, valHov, ringHov);
            }
            else if (c.kind == FxControl::Toggle)
                draw_ctl_toggle(g, t, ccx, knobY, 20.0f * k * CTL_FONT_K,
                                c.options[(size_t)mod_val_[mm][i]], hov);
            else {   // radio centred on the control row, or over the span if rowSpan>1
                const float ry = jy(cellJY + row_ctl_y(c.row) - 47.5f * (float)(c.rowSpan - 1));
                draw_ctl_radio(g, t, ccx, ry, k, c.options,
                               (int)mod_val_[mm][i], hov ? ctl_hover_opt_ : -1);
            }
            // Param label at the cell bottom — JUCE getLabelFont is REGULAR weight
            // (not bold like the toggle state text), CONTROL_LABEL brown, 14pt.
            g.set_font("Inter", S(14.0f * k * CTL_FONT_K));
            g.set_fill_color(hex_color(CONTROL_LABEL));
            const float w = g.measure_text(c.label);
            g.fill_text(c.label, X(ccx) - w * 0.5f, Y(labY));
        }
    }

    // Saturn-ring knob — a faithful port of SaturnRingKnob.cpp, scaled to diameter
    // `D` (SVG). Paint order: circle → ring arc (mod depth) → modulated dot →
    // value triangle. `depth` is the ring depth (−1..1), `macro` the module macro
    // that drives the dot's modulated position (clamp(value + macro·depth)).
    void draw_ctl_knob(cv::Canvas& g, const PanelTransform& t, float ccx, float ccy,
                       float D, float value, float depth, float macro,
                       bool valHov, bool ringHov) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        const float R = D * 0.5f, sc = D / 50.0f, stroke = 2.0f * sc;
        // JUCE getKnobColour / getRingColour: brighten the part that's hovered/dragged.
        const std::string knobCol = valHov ? brighten_hex(CONTROL_LABEL) : std::string(CONTROL_LABEL);
        const std::string ringCol = ringHov ? brighten_hex(CONTROL_LABEL) : std::string(CONTROL_LABEL);
        auto ang = [&](float v) { return 1.25f * CTL_PI + std::clamp(v, 0.0f, 1.0f) * 1.5f * CTL_PI; };
        const float ringR = R + 10.0f * sc;   // getRingRadius: knobR + kRingGap + ringStroke/2

        // 1) Circle outline.
        g.set_stroke_color(hex_color(knobCol));
        g.set_line_width(S(stroke));
        g.stroke_circle(X(ccx), Y(ccy), S(R - stroke * 0.5f));

        // 2) Saturn ring arc — from value to value+depth (only when |depth| > 0.01).
        if (std::abs(depth) > 0.01f) {
            const float a0 = ang(value), a1 = ang(std::clamp(value + depth, 0.0f, 1.0f));
            g.set_stroke_color(hex_color(ringCol));
            g.set_line_width(S(2.0f * sc)); g.set_line_cap(cv::LineCap::round);
            g.begin_path();
            for (int s = 0; s <= 32; ++s) {           // kArcSegments
                const float a = a0 + (a1 - a0) * (float)s / 32.0f;
                const float x = ccx + ringR * std::sin(a), y = ccy - ringR * std::cos(a);
                if (s == 0) g.move_to(X(x), Y(y)); else g.line_to(X(x), Y(y));
            }
            g.stroke_current_path();
        }

        // 3) Modulated dot — at clamp(value + macro·depth); shown on hover/drag or
        //    whenever depth is set (JUCE showModIndicator, no fade in the port).
        if (valHov || ringHov || std::abs(depth) > 0.01f) {
            const float a = ang(std::clamp(value + macro * depth, 0.0f, 1.0f));
            g.set_fill_color(hex_color(ringCol));
            g.fill_circle(X(ccx + ringR * std::sin(a)), Y(ccy - ringR * std::cos(a)), S(3.0f * sc));
        }

        // 4) Value triangle (tip outward).
        const float ang0 = ang(value);
        const float indR = R - stroke - 8.0f * sc;                    // triangle gap
        const float trcx = ccx + indR * std::sin(ang0), trcy = ccy - indR * std::cos(ang0);
        const float ah = 10.0f * sc * 0.6f, bw = 12.0f * sc * (2.0f / 3.0f);  // Cradle proportions
        auto rot = [&](float lx, float ly, float& ox, float& oy) {
            ox = trcx + lx * std::cos(ang0) - ly * std::sin(ang0);
            oy = trcy + lx * std::sin(ang0) + ly * std::cos(ang0);
        };
        float ax, ay, bx, by, cx2, cy2;
        rot(-bw * 0.5f, ah * 0.5f, ax, ay);    // base-left (toward centre)
        rot(bw * 0.5f, ah * 0.5f, bx, by);     // base-right
        rot(0.0f, -ah * 0.5f, cx2, cy2);       // tip (outward)
        g.set_fill_color(hex_color(knobCol));
        g.begin_path(); g.move_to(X(ax), Y(ay)); g.line_to(X(bx), Y(by));
        g.line_to(X(cx2), Y(cy2)); g.close_path(); g.fill_current_path();
    }

    // Two-state toggle: the current option text centred where the knob sits.
    // JUCE ToggleTextButton hover = brown #A19B92@0.5 rounded bg (75×55, r10) +
    // beige #F5F1ED state text; otherwise the text is the normal brown.
    void draw_ctl_toggle(cv::Canvas& g, const PanelTransform& t, float ccx, float ccy,
                         float fontSvg, const std::string& state, bool hovered) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        const float k = fx_k();
        g.set_font(DD_BOLD, S(fontSvg));             // JUCE ToggleTextButton fontSize (already +2 for Inter)
        const float w = g.measure_text(state);
        if (hovered) {   // pill ~JUCE 60×44 r8, but always wide enough for the text
            const float bw = std::max(S(60.0f * k), w + S(28.0f * k)), bh = S(44.0f * k);
            g.set_fill_color(hex_color(CONTROL_LABEL).with_alpha(0.5f));
            g.fill_rounded_rect(X(ccx) - bw * 0.5f, Y(ccy) - bh * 0.5f, bw, bh, S(8.0f * k));
        }
        g.set_fill_color(hex_color(hovered ? "#f5f1ed" : CONTROL_LABEL));
        bt(g, state, X(ccx) - w * 0.5f, Y(ccy + fontSvg * 0.36f), t.scale);
    }

    // Vertical radio group: circles Ø12 + label, spacing 17. The whole group
    // (circle + 18px gap + widest label) is CENTRED on `ccx`, and the param label
    // is also centred there — matching JUCE RadioButtonGroup (options + group
    // label both centred on the options' idealBounds).
    void draw_ctl_radio(cv::Canvas& g, const PanelTransform& t, float ccx, float ccy,
                        float k, const std::vector<std::string>& options, int sel, int hoverOpt,
                        float cdBase = 12.0f, float spBase = 17.0f) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        const float cd = cdBase * k, sp = spBase * k, lx = 18.0f * k, stroke = 1.5f * k;
        const int n = (int)options.size();
        g.set_font("Inter", S(12.0f * k * CTL_FONT_K));   // JUCE radio label 12 (regular)
        float maxW = 0.0f;
        for (const auto& o : options) maxW = std::max(maxW, g.measure_text(o));   // screen px
        const float idealW = lx + maxW / t.scale;                                 // SVG units
        const float circx = ccx - idealW * 0.5f + cd * 0.5f, labx = ccx - idealW * 0.5f + lx;
        const float startY = ccy - (float)(n - 1) * sp * 0.5f;
        for (int i = 0; i < n; ++i) {
            const float cy = startY + (float)i * sp;
            const std::string col = (i == hoverOpt) ? brighten_hex(CONTROL_LABEL) : std::string(CONTROL_LABEL);
            if (i == sel) { g.set_fill_color(hex_color(col)); g.fill_circle(X(circx), Y(cy), S(cd * 0.5f)); }
            else { g.set_stroke_color(hex_color(col)); g.set_line_width(S(stroke)); g.stroke_circle(X(circx), Y(cy), S(cd * 0.5f)); }
            g.set_fill_color(hex_color(col));
            g.fill_text(options[(size_t)i], X(labx), Y(cy + 4.0f * k));
        }
    }

    // Compressor gain-reduction meter — JUCE GainReductionMeter: 2 rows (L/R) ×
    // 15 circles (1dB each, Ø10 gap 2.5), centred on (mx, my). No audio in the
    // playground → all circles idle (#A19B92 @ 50%). Label centred below.
    void draw_gr_meter(cv::Canvas& g, const PanelTransform& t, float mx, float my,
                       float labY, const char* label) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        const float k = fx_k();
        const float cs = 10.0f * k, pitch = 12.5f * k;   // circleSize, circleSize+gap
        const int cols = 15, rows = 2;
        const float gw = (float)cols * cs + (float)(cols - 1) * 2.5f * k;
        const float gh = (float)rows * cs + 2.5f * k;
        const float gx0 = mx - gw * 0.5f, gy0 = my - gh * 0.5f;
        g.set_fill_color(hex_color(CONTROL_LABEL).with_alpha(0.5f));   // idle = inactive
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c)
                g.fill_circle(X(gx0 + (float)c * pitch + cs * 0.5f), Y(gy0 + (float)r * pitch + cs * 0.5f), S(cs * 0.5f));
        (void)labY; (void)label;   // GR meter has no label (per request)
    }

    // ── Input / Output level meters (LevelMeterFader) ────────────────────────
    // Two vertical L/R meter bars + a triangle drag handle, in the band's side
    // margins. Geometry is LevelMeterFaderConfig (8px bars, 120 tall, 12 triangle)
    // and EffectSelectorOverlay fader placement, all scaled by k.
    // The draggable value track of a meter fader (the full 50px column over the
    // bar height), in SVG coords. Input is on the left, output symmetric.
    Rect4 fader_track(bool input) const {
        const float k = fx_k();
        const float fx = input ? jx(30.0f) : jx(850.0f - 30.0f - 50.0f);
        const float barTop = jy(54.0f) + 6.0f * k;
        return { fx, barTop, fx + 50.0f * k, barTop + 120.0f * k };
    }
    float fader_value_from_y(float py, bool input) const {
        const Rect4 r = fader_track(input);
        return std::clamp(1.0f - (py - r.y0) / (r.y1 - r.y0), 0.0f, 1.0f);
    }

    // Seed module m's live values from the JUCE defaults, and pick its two slot
    // controls = the first two assignable params (getDefaultVisibleParams).
    void init_delay_values(int m) {
        if (m < 0 || m >= 8) return;
        const auto& cs = mod_ctls(m);
        for (size_t i = 0; i < cs.size() && i < 12; ++i) { mod_val_[m][i] = cs[i].def; mod_dep_[m][i] = cs[i].modDep; }
        // Default slot params = first two ASSIGNABLE controls (skip the GR meter).
        int found = 0;
        slot_vis_[m][0] = slot_vis_[m][1] = 0;
        for (int i = 0; i < (int)cs.size() && found < 2; ++i)
            if (cs[i].assignable()) slot_vis_[m][found++] = i;
    }

    // The module's macro value (0..1) drives every Saturn ring's modulated dot,
    // exactly like JUCE EffectControlPanel::setMacroValue — element 0..7 = module.
    float module_macro() const {
        if (fx_menu_module_ >= 0 && fx_menu_module_ < 8) return (float)element_value(fx_menu_module_);
        return 0.0f;
    }

    // Geometry shared by draw_fx_controls and hit-testing: cell top-left (JUCE px),
    // knob centre Y, cell-centre X for control `i`.
    void ctl_cell(int i, float& cellJX, float& cellJY, float& ccx, float& knobY) const {
        const auto& c = mod_ctls(fx_menu_module_)[(size_t)i];
        constexpr float CELL_W = 110.0f, CELL_H = 95.0f, GRID_W = 660.0f;
        const float gridX = (850.0f - GRID_W) * 0.5f;
        const float gridY = ((fx_panel().y1 - fx_panel().y0) / fx_k() - 2.0f * CELL_H) * 0.5f;
        cellJX = gridX + (float)(c.col - 1) * CELL_W;
        cellJY = gridY + (float)(c.row - 1) * CELL_H;
        ccx = jx(cellJX + CELL_W * 0.5f);
        // Control centre: row 1 = 36 (top), row 2 pushed DOWN to open the gap
        // between the two rows. Labels sit ROW_LAB below (50% closer than before).
        knobY = jy(cellJY + row_ctl_y(c.row));
    }
    // Top row pulled DOWN closer to the bottom row, then nudged UP 20px (≈15.2 local);
    // label gap ~43.5 from control.
    static float row_ctl_y(int row) { return row == 1 ? 34.8f : 44.0f; }
    static float row_lab_y(int row) { return row == 1 ? 78.3f : 87.5f; }

    // Control under (px,py): returns index + sets `opt` to the radio option (else -1).
    int ctl_at(float px, float py, int& opt) const {
        opt = -1;
        const float k = fx_k();
        const auto& cs = mod_ctls(fx_menu_module_);
        for (int i = 0; i < (int)cs.size(); ++i) {
            float cellJX, cellJY, ccx, knobY; ctl_cell(i, cellJX, cellJY, ccx, knobY);
            if (cs[i].kind == FxControl::Knob) {
                // JUCE hit zones: value circle (dist ≤ knobR−4), ring annulus
                // (knobR−4 … knobR+20) minus the bottom 90° dead zone. sc = k here.
                const float dx = px - ccx, dy = py - knobY, dist = std::sqrt(dx * dx + dy * dy);
                const float ringInner = 20.0f * k, ringOuter = 44.0f * k;   // (25−5)k, (25+19)k
                if (dist <= ringInner) { opt = 0; return i; }               // value zone
                if (dist <= ringOuter) {                                    // ring zone
                    const float a = std::atan2(dx, -dy);                    // 0 at 12 o'clock
                    if (std::abs(a) < 0.75f * CTL_PI) { opt = 1; return i; } // skip bottom dead zone
                }
            } else if (cs[i].kind == FxControl::Toggle) {
                if (px >= jx(cellJX + 6.0f) && px <= jx(cellJX + 104.0f) &&
                    py >= jy(cellJY + 4.0f) && py <= jy(cellJY + 70.0f)) return i;
            } else {   // radio: which option row (centred over the span if rowSpan>1)
                const float sp = 17.0f * k;
                const float ry = jy(cellJY + row_ctl_y(cs[i].row) - 47.5f * (float)(cs[i].rowSpan - 1));
                const int n = (int)cs[i].options.size();
                const float startY = ry - (float)(n - 1) * sp * 0.5f;
                for (int o = 0; o < n; ++o) {
                    if (std::abs(py - (startY + (float)o * sp)) < sp * 0.5f &&
                        px >= jx(cellJX + 6.0f) && px <= jx(cellJX + 104.0f)) { opt = o; return i; }
                }
            }
        }
        return -1;
    }

    void draw_fx_meters(cv::Canvas& g, const PanelTransform& t) const {
        const float k = fx_k();
        // EffectSelectorOverlay: input at x=30, output symmetric; both at y=54.
        draw_meter_fader(g, t, jx(30.0f), jy(54.0f), true,  "Input",  input_gain_,
                         meter_hover_ == 0 || fader_drag_ == 0);
        draw_meter_fader(g, t, jx(850.0f - 30.0f - 50.0f), jy(54.0f), false, "Output", output_gain_,
                         meter_hover_ == 1 || fader_drag_ == 1);
    }

    void draw_meter_fader(cv::Canvas& g, const PanelTransform& t, float fx, float fy,
                          bool triRight, const std::string& label, float value, bool hovered) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        const float k = fx_k();
        const float mW = 8.0f * k, mH = 120.0f * k, gap = 1.0f * k, off = 16.5f * k;
        const float topPad = 6.0f * k, triSz = 12.0f * k, triOff = 2.0f * k, rad = 1.0f * k;
        const float metersX = fx + off + (triRight ? 0.0f : (triSz + triOff));   // L meter left edge
        const float barTop = fy + topPad;
        // Two meter bars: background = #A19B92 @ 50% (no audio level in playground).
        g.set_fill_color(hex_color(CONTROL_LABEL).with_alpha(0.5f));
        g.fill_rounded_rect(X(metersX), Y(barTop), S(mW), S(mH), S(rad));
        g.fill_rounded_rect(X(metersX + mW + gap), Y(barTop), S(mW), S(mH), S(rad));
        // Triangle handle: tip tracks value over the bar height, pointing at the bars.
        const float tipY = barTop + (1.0f - value) * mH;
        const float apexX = triRight ? (metersX + 2.0f * mW + gap + triOff) : (fx + off);
        const float th = triSz, ty = tipY - th * 0.5f;
        g.set_fill_color(hex_color(hovered ? brighten_hex(CONTROL_LABEL) : std::string(CONTROL_LABEL)));
        g.begin_path();
        if (triRight) {   // apex (point) on the LEFT toward the bars, flat edge right
            g.move_to(X(apexX), Y(tipY));
            g.line_to(X(apexX + th), Y(ty));
            g.line_to(X(apexX + th), Y(ty + th));
        } else {          // apex on the RIGHT toward the bars, flat edge left
            g.move_to(X(apexX + th), Y(tipY));
            g.line_to(X(apexX), Y(ty));
            g.line_to(X(apexX), Y(ty + th));
        }
        g.close_path(); g.fill_current_path();
        // Label ("Input"/"Output") centred under the METER BARS (not the fader
        // column) so it reads centred on both sides regardless of triangle side.
        const float barsCenter = metersX + mW + gap * 0.5f;
        g.set_font("Inter", S(14.0f * k * CTL_FONT_K));   // getLabelFont — regular weight
        g.set_fill_color(hex_color(CONTROL_LABEL));
        const float w = g.measure_text(label);
        g.fill_text(label, X(barsCenter) - w * 0.5f, Y(barTop + mH + 16.0f * k));
    }

    // ── LFO advanced page (slot 0) ──────────────────────────────────────────
    // Geometry of LFO depth-slider row `i` (0..5), in JUCE-px (850-wide) space.
    // The 6 rows fill cols 4–6 of the 6×2 control grid (EditorLayout: slidersX =
    // controlPanelX + 3·CELL_W, width 3·CELL_W, height 2·CELL_H). Each row is
    // [minLabel 84 | name 50 | slider | maxLabel 84], 2px pad each side of slider.
    struct LfoRow { float cy, trackL, trackR, minLabX, nameCx, maxLabR; };
    LfoRow lfo_depth_row(int i) const {
        constexpr float CELL_W = 110.0f, CELL_H = 95.0f, GRID_W = 660.0f;
        constexpr float MIN_LAB_W = 84.0f, NAME_W = 50.0f, MAX_LAB_W = 84.0f, PAD_H = 2.0f;
        const float gridX = (850.0f - GRID_W) * 0.5f;
        const float gridY = ((fx_panel().y1 - fx_panel().y0) / fx_k() - 2.0f * CELL_H) * 0.5f;
        const float sx0 = gridX + 3.0f * CELL_W, sw = 3.0f * CELL_W;
        const float rowH = 2.0f * CELL_H / 6.0f;
        const float cy = gridY + (float)i * rowH + rowH * 0.5f;
        const float sliderX0 = sx0 + MIN_LAB_W + NAME_W + PAD_H;
        const float sliderX1 = sx0 + sw - MAX_LAB_W - PAD_H;
        return { cy, sliderX0 + 4.0f, sliderX1 - 4.0f, sx0,
                 sx0 + MIN_LAB_W + NAME_W * 0.5f, sx0 + sw };
    }

    void draw_lfo_depth_sliders(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        const float k = fx_k();
        auto JX = [&](float v) { return X(jx(v)); };
        auto JY = [&](float v) { return Y(jy(v)); };
        auto JS = [&](float v) { return S(v * k); };
        constexpr float R = 5.0f, kappa = 0.5522847498f;   // thumb radius, circular-arc bezier
        g.set_font("Inter", S(14.0f * k * CTL_FONT_K));
        for (int i = 0; i < 6; ++i) {
            const LfoRow r = lfo_depth_row(i);
            const float trackW = r.trackR - r.trackL;
            // Background track (lightBrown @ 0.5, 1px).
            g.set_fill_color(hex_color("#ded8cc").with_alpha(0.5f));
            g.fill_rounded_rect(JX(r.trackL), JY(r.cy) - JS(0.5f), JS(trackW), JS(1.0f), JS(0.5f));
            // Centre tick (0%) — medBrown @ 0.6, 1×10px.
            const float cX = r.trackL + trackW * 0.5f;
            g.set_fill_color(hex_color(CONTROL_LABEL).with_alpha(0.6f));
            g.fill_rect(JX(cX) - JS(0.5f), JY(r.cy) - JS(5.0f), JS(1.0f), JS(10.0f));
            // Thumb positions from bipolar values (-1..1 → 0..1 norm).
            const float minNorm = (lfo_depth_min_[i] + 1.0f) * 0.5f;
            const float maxNorm = (lfo_depth_max_[i] + 1.0f) * 0.5f;
            const float minX = r.trackL + minNorm * trackW, maxX = r.trackL + maxNorm * trackW;
            // Accent fill between the thumbs (medBrown, 1px).
            g.set_fill_color(hex_color(CONTROL_LABEL));
            if (maxX - minX > 0.5f)
                g.fill_rect(JX(minX), JY(r.cy) - JS(0.5f), JS(maxX - minX), JS(1.0f));
            // Min thumb — left half-circle (bulge left, flat right edge).
            auto halfCircle = [&](float cx, float dir) {   // dir = -1 left, +1 right
                g.begin_path();
                g.move_to(JX(cx), JY(r.cy) - JS(R));
                g.cubic_to(JX(cx + dir * R * kappa), JY(r.cy) - JS(R),
                           JX(cx + dir * R), JY(r.cy) - JS(R * kappa),
                           JX(cx + dir * R), JY(r.cy));
                g.cubic_to(JX(cx + dir * R), JY(r.cy) + JS(R * kappa),
                           JX(cx + dir * R * kappa), JY(r.cy) + JS(R),
                           JX(cx), JY(r.cy) + JS(R));
                g.close_path(); g.fill_current_path();
            };
            halfCircle(minX, -1.0f);
            halfCircle(maxX, +1.0f);
            // Labels: min% (left), name (centred), max% (right) — medBrown, regular.
            g.set_fill_color(hex_color(CONTROL_LABEL));
            const int minInt = (int)std::lround(lfo_depth_min_[i] * 100.0f);
            const int maxInt = (int)std::lround(lfo_depth_max_[i] * 100.0f);
            const std::string minTxt = (minInt > 0 ? "+" : "") + std::to_string(minInt) + "%";
            const std::string maxTxt = (maxInt > 0 ? "+" : "") + std::to_string(maxInt) + "%";
            const float baseY = r.cy + 14.0f * 0.32f;   // ~vertical centre for 14pt
            g.fill_text(minTxt, JX(r.minLabX), JY(baseY));                 // left-justified
            const float mw = g.measure_text(maxTxt);
            g.fill_text(maxTxt, JX(r.maxLabR) - mw, JY(baseY));            // right-justified
            const std::string& fx = module_fx_[(size_t)(i + 1)];
            const std::string name = std::to_string(i + 1) + ": " + (fx.empty() ? std::string("---") : fx);
            const float nw = g.measure_text(name);
            g.fill_text(name, JX(r.nameCx) - nw * 0.5f, JY(baseY));        // centred
        }
    }

    // LFO waveform visualiser — col 1, row 1 of the grid (replaces the input fader
    // for the LFO slot). JUCE LFOVisualizer: medBrown stroked waveform + a position
    // dot, 80×55 centred on that cell. Static sine here (no audio clock).
    void draw_lfo_visualizer(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        const float k = fx_k();
        constexpr float CELL_W = 110.0f, CELL_H = 95.0f, GRID_W = 660.0f, VIS_W = 80.0f, VIS_H = 55.0f, PAD = 4.0f;
        const float gridX = (850.0f - GRID_W) * 0.5f;
        const float gridY = ((fx_panel().y1 - fx_panel().y0) / fx_k() - 2.0f * CELL_H) * 0.5f;
        const float cellCx = gridX + CELL_W * 0.5f, cellCy = gridY + CELL_H * 0.5f;   // col1/row1 centre
        const float x0 = cellCx - VIS_W * 0.5f + PAD, w = VIS_W - 2.0f * PAD;
        const float cy = cellCy, amp = (VIS_H - 2.0f * PAD) * 0.45f;
        const int wf = (int)mod_val_[0][0];   // Wave radio: 0 Sine, 1 Square, 2 Saw
        auto wave = [&](float ph) -> float {
            if (wf == 1) return ph < 0.5f ? -1.0f : 1.0f;                       // Square
            if (wf == 2) { float s = ph + 0.5f; if (s >= 1.0f) s -= 1.0f; return s * 2.0f - 1.0f; }  // Saw
            return std::sin(ph * 2.0f * 3.14159265f);                          // Sine
        };
        g.set_stroke_color(hex_color(CONTROL_LABEL));
        g.set_line_width(S(2.0f * k)); g.set_line_cap(cv::LineCap::round);
        g.begin_path();
        const int N = 48;
        for (int i = 0; i <= N; ++i) {
            const float ph = (float)i / (float)N;
            const float px = jx(x0 + ph * w), py = jy(cy - wave(ph) * amp);
            if (i == 0) g.move_to(X(px), Y(py)); else g.line_to(X(px), Y(py));
        }
        g.stroke_current_path();
        // Position dot at phase 0 (start of the cycle).
        g.set_fill_color(hex_color(CONTROL_LABEL));
        g.fill_circle(X(jx(x0)), Y(jy(cy - wave(0.0f) * amp)), S(4.0f * k));
    }

    // Depth-slider hit-test (SVG px). Sets `row` (0..5) and `handle` (0 min / 1 max).
    bool lfo_depth_at(float svgx, float svgy, int& row, int& handle) const {
        const float k = fx_k();
        const float jX = (svgx - fx_panel().x0) / k, jY = (svgy - fx_panel().y0) / k;
        const float rowH = 2.0f * 95.0f / 6.0f;
        for (int i = 0; i < 6; ++i) {
            const LfoRow r = lfo_depth_row(i);
            if (std::abs(jY - r.cy) > rowH * 0.5f) continue;
            if (jX < r.trackL - 8.0f || jX > r.trackR + 8.0f) return false;
            const float trackW = r.trackR - r.trackL;
            const float minX = r.trackL + (lfo_depth_min_[i] + 1.0f) * 0.5f * trackW;
            const float maxX = r.trackL + (lfo_depth_max_[i] + 1.0f) * 0.5f * trackW;
            row = i;
            handle = (std::abs(jX - minX) <= std::abs(jX - maxX)) ? 0 : 1;
            return true;
        }
        return false;
    }
    // Move the active depth handle to the cursor x (SVG px); keeps min ≤ max.
    void lfo_depth_drag_to(float svgx) {
        if (lfo_depth_drag_ < 0) return;
        const int i = lfo_depth_drag_;
        const LfoRow r = lfo_depth_row(i);
        const float k = fx_k();
        const float jX = (svgx - fx_panel().x0) / k;
        const float trackW = r.trackR - r.trackL;
        const float v = std::clamp((jX - r.trackL) / trackW, 0.0f, 1.0f) * 2.0f - 1.0f;
        if (lfo_depth_handle_ == 0) lfo_depth_min_[i] = std::min(v, lfo_depth_max_[i]);
        else                        lfo_depth_max_[i] = std::max(v, lfo_depth_min_[i]);
    }

    // Replace every #A19B92 stroke/fill in an icon SVG with `hex` (for selection).
    static void recolor_svg(std::string& svg, const std::string& hex) {
        for (const char* from : {"#A19B92", "#a19b92"}) {
            size_t p = 0;
            while ((p = svg.find(from, p)) != std::string::npos) { svg.replace(p, 7, hex); p += hex.size(); }
        }
    }

    // Re-draw a knob's ring + needle brightened (hover affordance) on top. The
    // ring is its bezier 'C' fill within the bbox (not the L-only triangle).
    void overlay_brighten_ring(cv::Canvas& canvas, const PanelTransform& t,
                               const KnobHover& k, int index) const {
        const std::string lit = brighten_hex(k.ring);
        const std::string fillAttr = "fill=\"" + k.ring + "\"";
        std::string inner;
        for (size_t p = svg_copy_.find("<path "); p != std::string::npos;
             p = svg_copy_.find("<path ", p + 1)) {
            const size_t end = svg_copy_.find("/>", p);
            if (end == std::string::npos) break;
            std::string el = svg_copy_.substr(p, end - p + 2);
            const size_t fp = el.find(fillAttr);
            if (fp == std::string::npos || el.find('C') == std::string::npos) continue;
            float x, y;
            if (!first_move(el, x, y)) continue;
            if (std::abs(x - k.cx) > k.bbox || std::abs(y - k.cy) > k.bbox) continue;
            el.replace(fp, fillAttr.size(), "fill=\"" + lit + "\"");
            inner += el;
        }
        if (inner.empty()) return;
        inner += needle_svg(k, index, 1);   // brightened needle on top
        draw_mini(canvas, t, inner);
    }

    // ── Preset browser ───────────────────────────────────────────────────────
    static bool icontains(const std::string& hay, const std::string& needle) {
        if (needle.empty()) return true;
        auto lower = [](std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; };
        return lower(hay).find(lower(needle)) != std::string::npos;
    }

    // Indices into presets_ passing the active filters (tag OR-set, favourites,
    // search) — the same predicate the dropdown list and prev/next/random share.
    std::vector<int> filtered() const {
        std::vector<int> r;
        for (int i = 0; i < (int)presets_.size(); ++i) {
            const auto& p = presets_[(size_t)i];
            if (!active_tags_.empty() && !active_tags_.count(p.tag)) continue;
            if (fav_only_ && !favourites_.count(p.path)) continue;
            if (!search_.empty() && !icontains(p.name, search_)) continue;
            r.push_back(i);
        }
        return r;
    }

    // prev/next cycle the FILTERED list with wrap-around; random uses a shuffle
    // bag that never repeats until every (filtered) preset has played.
    void preset_step(int dir) {
        const auto f = filtered();
        if (f.empty()) return;
        int pos = -1;
        for (int j = 0; j < (int)f.size(); ++j) if (f[(size_t)j] == preset_idx_) { pos = j; break; }
        const int n = (int)f.size();
        pos = (pos < 0) ? (dir > 0 ? 0 : n - 1) : (pos + dir + n) % n;
        preset_idx_ = f[(size_t)pos];
        request_repaint();
    }
    void preset_random() {
        const auto f = filtered();
        if (f.empty()) return;
        if (f.size() == 1) { preset_idx_ = f[0]; request_repaint(); return; }
        if (bag_pos_ >= bag_.size() || bag_filter_n_ != (int)f.size()) {
            bag_ = f;                                   // bag over the filtered set
            std::shuffle(bag_.begin(), bag_.end(), rng_);
            if (bag_.front() == preset_idx_) std::swap(bag_.front(), bag_.back());
            bag_pos_ = 0; bag_filter_n_ = (int)f.size();
        }
        preset_idx_ = bag_[bag_pos_++];
        request_repaint();
    }

    // Draw the current preset name (or "No Preset") centred in the header, in the
    // baked glyphs' place. #a19b92, Inter, sized to the original ~13.5px caps.
    void draw_preset_name(cv::Canvas& canvas, const PanelTransform& t) const {
        const std::string label = (preset_idx_ >= 0 && preset_idx_ < (int)presets_.size())
                                       ? presets_[(size_t)preset_idx_].name : "No Preset";
        const bool pill = dropdown_open_ || hover_header_;
        canvas.set_font("Inter", 19.0f * t.scale);
        canvas.set_fill_color(hex_color(pill ? "#ffffff" : "#a19b92"));
        const float w = canvas.measure_text(label);
        const float vx = t.ox + 650.4f * t.scale - w * 0.5f;   // centre on the header
        const float vy = t.oy + 98.5f * t.scale;               // baseline
        if (pill) bt(canvas, label, vx, vy, t.scale); else canvas.fill_text(label, vx, vy);
    }

    static bool in_rect(const Rect4& r, float x, float y) {
        return x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1;
    }

    // The preset-browser dropdown: a popup panel (title + favourites/close, tag
    // filter chips, search box, scrollable preset rows). All canvas-drawn in
    // panel coords; chip/search/list rects are cached for hit-testing.
    static constexpr const char* DD_BOLD = "Inter";
    static constexpr const char* DD_TEXT = "#766f64";   // dark brown (kText)

    // Faux-bold: the bundle ships only Inter-Regular, so "Inter Bold" isn't
    // reliable across machines. Fill the run twice, offset a hair, to thicken it.
    void bt(cv::Canvas& g, const std::string& s, float vx, float vy, float scale) const {
        g.fill_text(s, vx, vy);
        g.fill_text(s, vx + 0.7f * scale, vy);
    }

    // A heart from cubic beziers (view coords) so width/height are independent —
    // the ♥ glyph is too tall. filled → solid, else a stroked outline.
    void draw_heart(cv::Canvas& g, float cx, float cy, float w, float hgt,
                    bool filled, const cv::Color& col, float lw) const {
        const float hw = w * 0.5f, hh = hgt * 0.5f;
        g.begin_path();
        g.move_to(cx, cy - hh * 0.30f);
        g.cubic_to(cx - hw * 0.20f, cy - hh, cx - hw, cy - hh * 0.72f, cx - hw, cy - hh * 0.08f);
        g.cubic_to(cx - hw, cy + hh * 0.46f, cx - hw * 0.36f, cy + hh * 0.72f, cx, cy + hh);
        g.cubic_to(cx + hw * 0.36f, cy + hh * 0.72f, cx + hw, cy + hh * 0.46f, cx + hw, cy - hh * 0.08f);
        g.cubic_to(cx + hw, cy - hh * 0.72f, cx + hw * 0.20f, cy - hh, cx, cy - hh * 0.30f);
        g.close_path();
        if (filled) { g.set_fill_color(col); g.fill_current_path(); }
        else { g.set_stroke_color(col); g.set_line_width(lw); g.stroke_current_path(); }
    }

    void draw_dropdown(cv::Canvas& g, const PanelTransform& t) const {
        auto X = [&](float x) { return t.ox + x * t.scale; };
        auto Y = [&](float y) { return t.oy + y * t.scale; };
        auto S = [&](float s) { return s * t.scale; };
        const float w = DD_X1 - DD_X0, h = DD_Y1 - DD_Y0;

        g.set_fill_color(hex_color("#fbf4e6"));
        g.fill_rounded_rect(X(DD_X0), Y(DD_Y0), S(w), S(h), S(13));
        g.set_stroke_color(hex_color("#a19b92")); g.set_line_width(S(1.5f));  // macro-label brown
        g.stroke_rounded_rect(X(DD_X0), Y(DD_Y0), S(w), S(h), S(13));

        // ── Header buttons: Save · User · Favorites · Open Folder, then ✕ ──────
        static const char* HB[4] = {"Save", "User", "Favorites", "Open Folder"};
        hdr_btn_rects_.clear();
        const float by = DD_Y0 + 16;
        float bx = DD_X0 + 18;
        for (int i = 0; i < 4; ++i) {
            g.set_font(DD_BOLD, S(16));
            const float bw = g.measure_text(HB[i]) / t.scale;
            const Rect4 r{bx - 8, by - 3, bx + bw + 8, by + 23};   // nudged up 2px
            hdr_btn_rects_.push_back(r);
            const bool act = (i == 1 && user_only_) || (i == 2 && fav_only_);
            const bool hov = dd_hover_hdr_ == i;
            if (act || hov) {
                g.set_fill_color(act ? hex_color("#7b6896") : hex_color("#7b6896").with_alpha(0.5f));
                g.fill_rounded_rect(X(r.x0), Y(r.y0), S(r.x1 - r.x0), S(r.y1 - r.y0), S(7));
            }
            g.set_font(DD_BOLD, S(16));
            g.set_fill_color(hex_color(act || hov ? "#ffffff" : DD_TEXT));
            bt(g, HB[i], X(bx), Y(by + 17), t.scale);
            bx += bw + 22;
        }
        close_rect_ = {DD_X1 - 42, by - 4, DD_X1 - 16, by + 22};
        {   // close ✕ drawn as two thick strokes (crisper than the glyph)
            const float xcx = DD_X1 - 30, xcy = by + 11, xr = 6.0f;
            g.set_stroke_color(hex_color(dd_hover_close_ ? DD_TEXT : "#a19b92"));
            g.set_line_width(S(2.6f)); g.set_line_cap(cv::LineCap::round);
            g.begin_path(); g.move_to(X(xcx - xr), Y(xcy - xr)); g.line_to(X(xcx + xr), Y(xcy + xr)); g.stroke_current_path();
            g.begin_path(); g.move_to(X(xcx + xr), Y(xcy - xr)); g.line_to(X(xcx - xr), Y(xcy + xr)); g.stroke_current_path();
        }

        // ── Search box (white, magnifier glyph) ──────────────────────────────
        const float sy = DD_Y0 + 50;
        search_rect_ = {DD_X0 + 18, sy, DD_X1 - 18, sy + 34};
        g.set_fill_color(hex_color("#fbf4e6"));   // same beige as the panel
        g.fill_rounded_rect(X(search_rect_.x0), Y(sy), S(search_rect_.x1 - search_rect_.x0), S(34), S(9));
        // Border: rest light, hover darkens (the "darker stroke" state), focus accent.
        g.set_stroke_color(hex_color(search_focused_ ? "#7b6896" : (hover_search_ ? "#a19b92" : "#e0dccf")));
        g.set_line_width(S(search_focused_ ? 2.0f : (hover_search_ ? 1.7f : 1.3f)));
        g.stroke_rounded_rect(X(search_rect_.x0), Y(sy), S(search_rect_.x1 - search_rect_.x0), S(34), S(9));
        g.set_stroke_color(hex_color(hover_search_ || search_focused_ ? "#766f64" : "#b3ada0"));
        g.set_line_width(S(1.8f));
        g.stroke_circle(X(search_rect_.x0 + 20), Y(sy + 16), S(6.0f));
        g.begin_path();
        g.move_to(X(search_rect_.x0 + 24.5f), Y(sy + 20.5f));
        g.line_to(X(search_rect_.x0 + 28.5f), Y(sy + 24.5f));
        g.stroke_current_path();
        g.set_font("Inter", S(18));   // larger search text
        const float tx = search_rect_.x0 + 38;
        const bool ph = search_.empty() && !search_focused_;
        const int cl = std::clamp(caret_, 0, (int)search_.size());
        // Selection highlight behind the selected run.
        if (search_focused_ && has_sel()) {
            const float xa = tx + g.measure_text(search_.substr(0, (size_t)sel_lo())) / t.scale;
            const float xb = tx + g.measure_text(search_.substr(0, (size_t)sel_hi())) / t.scale;
            g.set_fill_color(hex_color("#7b6896").with_alpha(0.30f));
            g.fill_rounded_rect(X(xa - 1), Y(sy + 5), S(xb - xa + 2), S(24), S(2));
        }
        g.set_fill_color(hex_color(ph ? "#b3ada0" : DD_TEXT));
        g.fill_text(ph ? "Search…" : search_, X(tx), Y(sy + 23));
        // Taller caret at the caret index, blinking (frame-counter, resets on edit).
        if (search_focused_) {
            ++caret_blink_;
            if ((caret_blink_ / 32) % 2 == 0) {
                const float cx = tx + g.measure_text(search_.substr(0, (size_t)cl)) / t.scale;
                g.set_stroke_color(hex_color("#7b6896")); g.set_line_width(S(2.0f));
                g.begin_path();
                g.move_to(X(cx), Y(sy + 6)); g.line_to(X(cx), Y(sy + 28));
                g.stroke_current_path();
            }
            const_cast<DdfxEditorView*>(this)->request_repaint();   // drive the blink
        }

        // ── Tag filter chips (filled pastel; wrap when the row is full) ───────
        chip_rects_.clear();
        float cxp = DD_X0 + 18, cyp = sy + 34 + 11;
        for (size_t i = 0; i < tags_.size(); ++i) {
            g.set_font(DD_BOLD, S(17));
            const float cw = g.measure_text(tags_[i]) / t.scale + 24.0f;
            if (cxp + cw > DD_X1 - 18) { cxp = DD_X0 + 18; cyp += 36; }
            const Rect4 r{cxp, cyp, cxp + cw, cyp + 29};
            chip_rects_.push_back(r);
            const bool act = active_tags_.count(tags_[i]) > 0;
            const bool hov = dd_hover_chip_ == (int)i;
            const std::string col = tag_color(tags_[i]);
            g.set_fill_color(hex_color(col).with_alpha(act ? 1.0f : (hov ? 0.7f : 0.4f)));
            g.fill_rounded_rect(X(r.x0), Y(r.y0), S(cw), S(29), S(8));
            g.set_font(DD_BOLD, S(17));
            g.set_fill_color(hex_color(act || hov ? "#ffffff" : DD_TEXT));
            bt(g, tags_[i], X(r.x0 + 12), Y(r.y0 + 20), t.scale);
            cxp += cw + 8;
        }
        list_top_ = cyp + 29 + 11;

        // ── Scrollable preset list ───────────────────────────────────────────
        g.save();
        g.clip_rect(X(DD_X0 + 4), Y(list_top_), S(w - 8), S(DD_Y1 - list_top_ - 8));
        const auto f = filtered();
        for (size_t j = 0; j < f.size(); ++j) {
            const float ry = list_top_ + (float)j * DD_ROW_H - scroll_;
            if (ry + DD_ROW_H < list_top_ || ry > DD_Y1) continue;
            const int pi = f[j];
            const auto& p = presets_[(size_t)pi];
            const bool sel = pi == preset_idx_, hov = pi == dd_hover_row_;
            if (sel || hov) {   // highlight stops short of the heart (even padding)
                g.set_fill_color(sel ? hex_color("#7b6896") : hex_color("#7b6896").with_alpha(0.5f));
                g.fill_rounded_rect(X(DD_X0 + 10), Y(ry + 2), S((DD_X1 - 54) - (DD_X0 + 10)), S(DD_ROW_H - 4), S(7));
            }
            g.set_fill_color(hex_color(tag_color(p.tag)));
            g.fill_circle(X(DD_X0 + 28), Y(ry + DD_ROW_H * 0.5f), S(5));
            g.set_font(DD_BOLD, S(18));
            g.set_fill_color(hex_color(sel || hov ? "#ffffff" : DD_TEXT));
            bt(g, p.name, X(DD_X0 + 44), Y(ry + DD_ROW_H * 0.5f + 6), t.scale);
            // Heart sits OUTSIDE the highlight (on the panel). Resting outline is
            // light; hovering the heart darkens it to the parameter-name brown.
            const bool fav = favourites_.count(p.path) > 0;
            const cv::Color hcol = fav ? hex_color("#7b6896")
                                       : hex_color(dd_hover_heart_ == pi ? DD_TEXT : "#cdc6ba");
            draw_heart(g, X(DD_X1 - 34), Y(ry + DD_ROW_H * 0.5f), S(17), S(17), fav, hcol, S(1.8f));
        }
        g.restore();

        // ── Scrollbar (right edge): fades in/out as the pointer enters/leaves ──
        const float content = (float)f.size() * DD_ROW_H;
        const float view = DD_Y1 - list_top_ - 8.0f;
        // Ease the fade toward the hover target each frame; re-request paint until
        // it settles (exponential ease-out, snaps when within 0.01).
        const float target = (hover_menu_ && content > view) ? 1.0f : 0.0f;
        if (std::abs(scrollbar_alpha_ - target) > 0.01f) {
            scrollbar_alpha_ += (target - scrollbar_alpha_) * 0.22f;
            const_cast<DdfxEditorView*>(this)->request_repaint();
        } else {
            scrollbar_alpha_ = target;
        }
        if (content > view) {
            const float track_h = view;
            const float thumb_h = std::max(30.0f, track_h * view / content);
            const float maxscroll = content - view;
            const float ty = list_top_ + (maxscroll <= 0 ? 0 : scroll_ / maxscroll) * (track_h - thumb_h);
            const float SBW = 9.0f;                       // 30% wider than before (7)
            const float tx = DD_X1 - 15;                  // left edge (≈6px from panel edge)
            scroll_thumb_ = {tx - 3, ty, tx + SBW + 3, ty + thumb_h};   // cached for dragging
            if (scrollbar_alpha_ > 0.01f) {
                const bool active = hover_scrollbar_ || dragging_scroll_;
                g.set_fill_color(hex_color(active ? "#766f64" : "#a19b92")
                                     .with_alpha((active ? 0.85f : 0.6f) * scrollbar_alpha_));
                g.fill_rounded_rect(X(tx), Y(ty), S(SBW), S(thumb_h), S(4.5f));
            }
        } else {
            scroll_thumb_ = {};
        }
    }

    // The preset index under (px,py) in the list, or -1; sets on_heart if over
    // the row's favourite glyph.
    int dd_row_at(float px, float py, bool& on_heart) const {
        on_heart = false;
        if (py < list_top_ || py > DD_Y1 - 8 || px < DD_X0 + 8 || px > DD_X1 - 8) return -1;
        const auto f = filtered();
        const int j = (int)((py - list_top_ + scroll_) / DD_ROW_H);
        if (j < 0 || j >= (int)f.size()) return -1;
        if (px > DD_X1 - 46) on_heart = true;
        return f[(size_t)j];
    }

    // Handle a click inside the dropdown's interactive chrome. Returns true if it
    // hit something (close, fav, a tag chip, the search box, a row, or a heart).
    bool handle_dropdown_click(float px, float py) {
        search_focused_ = false;
        if (in_rect(close_rect_, px, py)) { dropdown_open_ = false; request_repaint(); return true; }
        for (size_t i = 0; i < hdr_btn_rects_.size(); ++i)
            if (in_rect(hdr_btn_rects_[i], px, py)) {
                if (i == 1) user_only_ = !user_only_;       // User (all presets are user → visual)
                else if (i == 2) { fav_only_ = !fav_only_; invalidate_bag(); }
                // Save (0) / Open Folder (3): no backing action in the playground.
                request_repaint(); return true;
            }
        for (size_t i = 0; i < chip_rects_.size(); ++i)
            if (in_rect(chip_rects_[i], px, py)) {
                const auto& tg = tags_[i];
                if (active_tags_.count(tg)) active_tags_.erase(tg); else active_tags_.insert(tg);
                invalidate_bag(); scroll_ = 0.0f; request_repaint(); return true;
            }
        if (in_rect(search_rect_, px, py)) {
            search_focused_ = true; caret_ = (int)search_.size(); sel_ = -1; caret_blink_ = 0;
            request_repaint(); return true;
        }
        bool heart = false;
        const int pi = dd_row_at(px, py, heart);
        if (pi >= 0) {
            if (heart) {
                const auto& path = presets_[(size_t)pi].path;
                if (favourites_.count(path)) favourites_.erase(path); else favourites_.insert(path);
                invalidate_bag();
            } else {
                preset_idx_ = pi;   // select; keep the menu open for auditioning
            }
            request_repaint(); return true;
        }
        return false;
    }

    void invalidate_bag() { bag_pos_ = bag_.size(); bag_filter_n_ = -1; }

    void update_dropdown_hover(float px, float py) {
        last_hover_px_ = px; last_hover_py_ = py;   // remembered so a wheel-scroll can re-hover
        const bool menu = in_rect({DD_X0, DD_Y0, DD_X1, DD_Y1}, px, py);
        if (menu != hover_menu_) { hover_menu_ = menu; request_repaint(); }   // drives scrollbar fade
        const bool sb = in_rect(scroll_thumb_, px, py);
        if (sb != hover_scrollbar_) { hover_scrollbar_ = sb; request_repaint(); }
        const bool srch = in_rect(search_rect_, px, py);
        if (srch != hover_search_) { hover_search_ = srch; request_repaint(); }
        int row = -1, chip = -1, heart = -1, hdr = -1;
        const bool cl = in_rect(close_rect_, px, py);
        for (size_t i = 0; i < hdr_btn_rects_.size(); ++i)
            if (in_rect(hdr_btn_rects_[i], px, py)) hdr = (int)i;
        for (size_t i = 0; i < chip_rects_.size(); ++i)
            if (in_rect(chip_rects_[i], px, py)) chip = (int)i;
        bool on_heart = false;
        const int pi = dd_row_at(px, py, on_heart);
        if (pi >= 0) { if (on_heart) heart = pi; else row = pi; }
        if (row != dd_hover_row_ || chip != dd_hover_chip_ || heart != dd_hover_heart_ ||
            cl != dd_hover_close_ || hdr != dd_hover_hdr_) {
            dd_hover_row_ = row; dd_hover_chip_ = chip; dd_hover_heart_ = heart;
            dd_hover_close_ = cl; dd_hover_hdr_ = hdr; request_repaint();
        }
    }

    // on_wheel only fires when this returns true (the platform routes the wheel
    // to value-widgets first); claim it whenever the browser is open.
    bool wants_wheel_value() const override { return dropdown_open_; }

    void on_wheel(float delta_y) override {
        if (!dropdown_open_) return;
        float maxscroll, travel; (void)travel;
        scroll_metrics(maxscroll, travel);
        scroll_ = std::clamp(scroll_ + delta_y * 1.0f, 0.0f, maxscroll);   // +down (px deltas)
        if (last_hover_px_ >= 0) update_dropdown_hover(last_hover_px_, last_hover_py_);  // re-hover
        request_repaint();
    }

    void scroll_metrics(float& maxscroll, float& thumb_travel) const {
        const float content = (float)filtered().size() * DD_ROW_H;
        const float view = DD_Y1 - list_top_ - 8.0f;
        maxscroll = std::max(0.0f, content - view);
        const float thumb_h = std::max(30.0f, view * view / std::max(content, 1.0f));
        thumb_travel = std::max(1.0f, view - thumb_h);
    }

    // Search field input (only while the dropdown is open and the box is focused).
    // ── Search field editing model (caret + selection) ───────────────────────
    bool has_sel() const { return sel_ >= 0 && sel_ != caret_; }
    int sel_lo() const { return std::min(sel_, caret_); }
    int sel_hi() const { return std::max(sel_, caret_); }
    void delete_sel() { const int a = sel_lo(), b = sel_hi(); search_.erase((size_t)a, (size_t)(b - a)); caret_ = a; sel_ = -1; }
    static bool is_word(char c) { return std::isalnum((unsigned char)c) != 0; }
    int word_left(int p) const { while (p > 0 && !is_word(search_[(size_t)p - 1])) --p; while (p > 0 && is_word(search_[(size_t)p - 1])) --p; return p; }
    int word_right(int p) const { const int n = (int)search_.size(); while (p < n && !is_word(search_[(size_t)p])) ++p; while (p < n && is_word(search_[(size_t)p])) ++p; return p; }
    void after_edit() { invalidate_bag(); scroll_ = 0.0f; caret_blink_ = 0; request_repaint(); }

    void on_text_input(const vw::TextInputEvent& e) override {
        if (!dropdown_open_ || !search_focused_) return;
        std::string ins;
        for (char c : e.text) if ((unsigned char)c >= 32) ins += c;
        if (ins.empty()) return;
        if (has_sel()) delete_sel();
        caret_ = std::clamp(caret_, 0, (int)search_.size());
        search_.insert((size_t)caret_, ins);
        caret_ += (int)ins.size(); sel_ = -1;
        after_edit();
    }

    bool on_key_event(const vw::KeyEvent& e) override {
        if (!dropdown_open_ || !e.is_down) return false;
        if (e.key == vw::KeyCode::escape) { dropdown_open_ = false; request_repaint(); return true; }
        if (!search_focused_) return false;
        using K = vw::KeyCode;
        const int n = (int)search_.size();
        caret_ = std::clamp(caret_, 0, n);
        if (e.isCmdDown() && e.key == K::a) { sel_ = 0; caret_ = n; request_repaint(); return true; }
        switch (e.key) {
            case K::backspace:
                if (has_sel()) delete_sel();
                else if (caret_ > 0) { search_.erase((size_t)(caret_ - 1), 1); --caret_; }
                after_edit(); return true;
            case K::delete_:
                if (has_sel()) delete_sel();
                else if (caret_ < n) search_.erase((size_t)caret_, 1);
                after_edit(); return true;
            case K::left:
                caret_ = e.isCmdDown() ? 0 : (e.isAltDown() ? word_left(caret_)
                                          : (has_sel() ? sel_lo() : std::max(0, caret_ - 1)));
                sel_ = -1; caret_blink_ = 0; request_repaint(); return true;
            case K::right:
                caret_ = e.isCmdDown() ? n : (e.isAltDown() ? word_right(caret_)
                                          : (has_sel() ? sel_hi() : std::min(n, caret_ + 1)));
                sel_ = -1; caret_blink_ = 0; request_repaint(); return true;
            case K::home: caret_ = 0; sel_ = -1; caret_blink_ = 0; request_repaint(); return true;
            case K::end_: caret_ = n; sel_ = -1; caret_blink_ = 0; request_repaint(); return true;
            default: return false;
        }
    }

    std::string svg_copy_;
    float svg_w_ = 0.0f, svg_h_ = 0.0f;
    RadioInfo radio_;
    std::vector<KnobHover> knobs_;
    std::vector<HitTarget> hits_;
    std::vector<Preset> presets_;
    int preset_idx_ = -1;
    std::vector<int> bag_;
    size_t bag_pos_ = 0;
    int bag_filter_n_ = -1;
    std::mt19937 rng_;
    int hover_knob_ = -1, hover_radio_ = -1, hover_hit_ = -1;

    // Dropdown browser (Phase 2)
    std::vector<std::string> tags_;                  // unique tags, sorted
    std::vector<std::string> tag_colors_;            // parallel to tags_
    std::vector<FxType> fx_types_;                   // effect grid (label + icon svg)
    bool dropdown_open_ = false;
    std::set<std::string> active_tags_;              // tag filter (OR logic)
    bool fav_only_ = false;
    bool user_only_ = false;
    std::set<std::string> favourites_;               // by preset path
    std::string search_;
    bool search_focused_ = false;
    int caret_ = 0;                  // caret index into search_
    int sel_ = -1;                   // selection anchor (-1 = none)
    mutable int caret_blink_ = 0;    // paint-frame counter for the blink
    float scroll_ = 0.0f;                             // list scroll (panel px)
    int dd_hover_row_ = -1;                          // hovered preset index (into presets_)
    int dd_hover_chip_ = -1;                         // hovered tag chip (into tags_)
    int dd_hover_heart_ = -1;                        // hovered heart (preset index)
    int dd_hover_hdr_ = -1;                          // hovered header button (0..3)
    bool dd_hover_close_ = false;
    bool hover_header_ = false;                      // header pill (when closed)

    // Dropdown layout (panel coords). Chip/search/list/button rects are measured
    // in paint() and cached here so hit-testing (no canvas) reads the same geom.
    // Sized in SVG space (1300 wide); JUCE's px are in its ~1004-wide editor, so
    // everything is ×1.3 to match the browser's on-screen proportions.
    static constexpr float DD_X0 = 432, DD_X1 = 840, DD_Y0 = 122, DD_Y1 = 692;
    static constexpr float DD_ROW_H = 34;
    mutable std::vector<Rect4> chip_rects_, hdr_btn_rects_;
    mutable Rect4 close_rect_{}, search_rect_{}, scroll_thumb_{};
    mutable float list_top_ = DD_Y0 + 150;
    mutable float scrollbar_alpha_ = 0.0f;           // fades in/out on menu hover
    bool hover_menu_ = false;                        // pointer is over the panel
    bool dragging_scroll_ = false;
    bool hover_scrollbar_ = false;
    bool hover_search_ = false;
    float drag_scroll_y0_ = 0, drag_scroll_s0_ = 0;  // drag anchor (py, scroll_)
    float last_hover_px_ = -1, last_hover_py_ = -1;  // last pointer pos (for scroll re-hover)

    // FX reorder: drag a middle module by its name "---" (slots 1-6); LFO/MASTER
    // pinned. The whole module (slot panel above + knob below) lifts into a ghost.
    int reorder_cand_ = -1;          // module a press started on (1-6), or -1
    bool reorder_active_ = false;    // a drag is in progress (or settling)
    bool dropping_ = false;          // released → animating the ghost into its slot
    float reorder_sx_ = 0, reorder_sy_ = 0;  // grab point (panel coords)
    float ghost_dxs_ = 0, ghost_dxk_ = 0;    // ghost slot / knob x-offset from rest
    float scale_ = 1.0f;             // ghost scale (eases up on grab — "lifts toward you")
    int drop_target_ = -1;           // insertion slot 1-6
    float slide_[8] = {0, 0, 0, 0, 0, 0, 0, 0};  // animated slot-shift per module (-1..1)
    int anim_sub_ = -1;              // frame-clock subscription (keeps the 60fps timer alive)
    float drop_t_ = 0;               // drop tween progress 0..1
    int settle_frames_ = 0;          // after commit: redraw the affected macro knobs for a few
    int settle_lo_ = 0, settle_hi_ = 0;   // frames so the base render doesn't flash a stale value
    int land_glow_m_ = -1;           // dropped slot: its bg eases from the lift colour back to normal
    float land_glow_t_ = 0.0f;       // glow strength (1 → 0)
    // drop tween start/end (slot pitch 140, knob pitch 130 → separate targets)
    float drop_dxs0_ = 0, drop_dxs1_ = 0, drop_dxk0_ = 0, drop_dxk1_ = 0, drop_s0_ = 1;
    static constexpr float SLOT_X0[8] = {91, 231, 372, 512, 653, 793, 933, 1074};
    static constexpr float SLOT_X1[8] = {226, 367, 507, 647, 788, 928, 1069, 1209};
    static constexpr float REORDER_SCALE = 1.07f, DROP_DUR = 0.20f;

    // FX-type picker: click a module's "---" name → a centred icon-grid overlay.
    int fx_menu_module_ = -1;        // module the overlay is open for, or -1
    int fx_view_ = 0;                // 0 = icon picker, 1 = effect Controls view
    float input_gain_ = 0.85f;       // Controls-view input meter fader (0..1)
    float output_gain_ = 0.85f;      // Controls-view output meter fader (0..1)
    int fader_drag_ = -1;            // dragging a meter handle: 0 = input, 1 = output
    float mod_val_[8][12] = {{0}};   // per-module live values (knob 0..1, toggle/radio = index)
    float mod_dep_[8][12] = {{0}};   // per-module Saturn-ring mod depths (-1..1)
    float lfo_depth_min_[6] = {0};   // LFO depth sliders: bipolar min handle per slot 1..6 (-1..1)
    float lfo_depth_max_[6] = {0};   // LFO depth sliders: bipolar max handle per slot 1..6 (-1..1)
    int   lfo_depth_drag_ = -1;      // dragging a depth slider (0..5), else -1
    int   lfo_depth_handle_ = 0;     // 0 = min handle, 1 = max handle, 2 = both (range)
    int slot_vis_[8][2] = {{0}};     // the two params each slot shows (indices into the effect table)
    int slot_hover_m_ = -1, slot_hover_pos_ = -1, slot_hover_ring_ = 0;   // hovered slot control
    int slot_title_hover_ = -1;      // module whose slot-title (icon) is hovered → show "Edit..."
    float slot_hover_t_[8] = {0};    // per-slot eased hover (0→1) for the "+"/"Add Effect" fade
    int wiggle_module_ = -1;         // module in iPhone-style "edit/wiggle" mode (-1 none)
    int pre_press_wiggle_ = -1;      // which module was wiggling at the start of the current press
    int wiggle_fade_m_ = -1;         // a module easing OUT of wiggle (settling to rest)
    float wiggle_fade_ = 0.0f;       // its decaying amplitude (1 → 0)
    int swap_anim_m_ = -1;           // module playing the Swap-Parameters animation
    float swap_anim_t_ = 0.0f;       // its progress (0 → 1)
    float anim_clock_ = 0.0f;        // free-running seconds (Edit-dots + wiggle phase)
    bool press_on_label_ = false;    // a slot press landed on the bottom name (→ advanced), not the icon
    bool lfo_press_ = false;         // a press started inside the pinned LFO slot (0) → opens its advanced page
    int slot_drag_m_ = -1, slot_drag_pos_ = -1; bool slot_drag_ring_ = false;  // dragging a slot knob
    // Right-click param picker (customize which param a slot position shows).
    int slot_menu_m_ = -1, slot_menu_pos_ = -1, slot_menu_hover_ = -2;
    int slot_menu_hover_badge_ = 0;   // hovered badge on a param row: 0 none, 1 = "1", 2 = "2"
    float smenu_x_t_ = 0.0f;          // header close-X hover-grow (0 → 1)
    float slot_menu_x_ = 0, slot_menu_y_ = 0;
    int knob_drag_ = -1;             // control index whose knob is being dragged, or -1
    bool knob_drag_ring_ = false;    // true = dragging the ring (depth), false = the value
    float drag_y0_ = 0, drag_v0_ = 0, drag_dep0_ = 0;  // drag anchors (start py, value, depth)
    int ctl_hover_ = -1;             // hovered control index (0..11), or -1
    int ctl_hover_opt_ = -1;         // hovered radio option within ctl_hover_, or -1
    int meter_hover_ = -1;           // hovered meter: 0 input, 1 output, -1 none
    // SaturnRingKnob kDragSensitivity 0.002/editor-px ÷ 1.3 (editor→SVG) = per SVG px.
    static constexpr float KNOB_DRAG_SENS = 0.002f / 1.3f;
    int fx_hover_ = -1;              // hovered grid cell (0..11)
    float fx_alpha_ = 0;             // overlay fade 0..1
    bool fx_closing_ = false;        // fading out → finalize when alpha hits 0
    float close_t_ = 0, remove_t_ = 0;   // hover progress for the close / remove buttons (grow + text)
    std::string module_fx_[8];       // chosen effect per module ("" = "---")
    float fx_cell_t_[12] = {0};      // per-cell hover progress 0..1 (eased to grow the icon)
    static constexpr float FX_HOVER_SCALE = 1.18f;   // hovered icon grows to this
    static constexpr float FX_HOVER_DUR = 0.13f;     // seconds for the ease in/out
};

std::unique_ptr<vw::View> KnobProcessor::create_view() {
#ifdef DDFX_SVG
    std::string svg = read_file(DDFX_SVG);
    if (svg.empty()) return nullptr;
    fix_stroked_rings(svg);                       // rings: concentric annular → evenodd
    strip_region(svg, "#a19b92", 600, 80, 700, 105);  // drop baked "No Preset" glyphs
    auto knobs = wire_macro_knobs(svg);           // the 8 macro knobs
    MacroKnobInfo thr;                            // + the Threshold/SaturnRingKnob
    const bool has_thr = wire_threshold_knob(svg, thr);
    if (has_thr) knobs.push_back(thr);
    RadioInfo radio = wire_radio(svg);            // Sine/Square/Saw (un-bakes the fill)

    std::vector<vw::DesignFrameElement> controls;
    std::vector<KnobHover> hovers;
    for (size_t i = 0; i < knobs.size(); ++i) {
        const auto& m = knobs[i];
        const bool is_threshold = has_thr && i + 1 == knobs.size();
        const float hit = is_threshold ? 30.0f : 52.0f;   // Saturn ring smaller
        vw::DesignFrameElement k;
        k.kind = vw::DesignFrameElement::Kind::knob;
        k.cx = m.cx; k.cy = m.cy;
        k.hit_radius = hit;
        k.needle_d = m.needle_id;     // the tagged triangle the SDK rotates
        k.value = 0.0f;               // rest at value 0 (7 o'clock); drag up → 5 o'clock
        controls.push_back(std::move(k));
        // Hover: brighten the knob's cream ring (Threshold's ring is #a19b92)
        // plus its needle (re-drawn at the knob's live value, read in paint).
        hovers.push_back({m.cx, m.cy, hit, hit + 8.0f,
                          is_threshold ? std::string("#a19b92") : std::string("#e8e1d5"),
                          m.needle_id});
    }

    // Top-bar icon buttons (hover-brighten + clickable) and the 8 macro bypass
    // dots (toggle + dim). Geometry from the captured SVG (panel coords); boxes
    // are padded past the tiny glyphs so they're comfortable to hit.
    std::vector<HitTarget> hits;
    hits.push_back({478.0f,  92.0f, 16.0f, 16.0f, "preset_prev"});
    hits.push_back({510.0f,  92.0f, 16.0f, 16.0f, "preset_next"});
    hits.push_back({790.0f,  92.0f, 18.0f, 16.0f, "dice"});
    hits.push_back({1199.0f, 92.0f, 20.0f, 18.0f, "gear"});
    // Each macro knob has a bypass dot at its upper-left (the dot/label sit ~47px
    // left of the knob centre; both rows are 130px pitch). The module's slot
    // panel above is a SEPARATE 140px-pitch row. A bypassed module fades two
    // rects: the slot (its own x) and the dot+knob+label band centred on the
    // KNOB (±55 reaches the dot/label on the left). Disjoint Y → no double-fade.
    const float dotXs[8] = {147.6f, 277.6f, 407.6f, 537.5f, 667.5f, 797.5f, 927.5f, 1057.5f};
    const float slotX[8][2] = {{91, 226}, {231, 367}, {372, 507}, {512, 647},
                               {653, 788}, {793, 928}, {933, 1069}, {1074, 1209}};
    for (int i = 0; i < 8 && i < (int)knobs.size(); ++i) {
        HitTarget d;
        d.cx = dotXs[i]; d.cy = 471.2f; d.hw = 10.0f; d.hh = 10.0f;   // click = the dot
        d.id = "bypass_" + std::to_string(i);
        d.toggle = true; d.on = true; d.knob_index = i;
        d.dim.push_back({slotX[i][0], 154.0f, slotX[i][1], 442.0f});            // slot panel
        d.dim.push_back({knobs[i].cx - 55.0f, 442.0f, knobs[i].cx + 55.0f, 628.0f});  // dot+knob+label
        hits.push_back(std::move(d));
    }

    // Real presets on disk: the DreamDateFX preset dir (subfolders = tags).
    std::vector<Preset> presets;
    std::vector<std::string> tags;
    if (const char* home = std::getenv("HOME")) {
        const std::string dir = std::string(home) +
            "/Library/Application Support/Dream Date Designs/DreamDateFX/Presets";
        presets = scan_presets(dir);
        tags = scan_tags(dir);
    }

    // Effect-picker grid: label + icon SVG (Docs/Design/Icons/Generated in DDIF).
    std::vector<FxType> fx_types;
    {
        const std::string icondir = "/Volumes/Projects/Dream Date Designs/Dream Date "
            "Instrument Framework/Docs/Design/Icons/Generated/";
        const std::pair<const char*, const char*> DEF[] = {
            {"Compressor", "compressor"}, {"EQ", "eq"}, {"Filter", "filter"},
            {"Distortion", "distortion"}, {"Bit Crusher", "bitreduction"}, {"Ring Mod", "ringmod"},
            {"Modulation", "modulation"}, {"Width", "tremolo"}, {"Delay", "delay"},
            {"Lo-Fi", "lofi"}, {"Reverb", "reverb"}, {"Space", "spaces"}};
        for (const auto& d : DEF)
            fx_types.push_back({d.first, read_file((icondir + d.second + ".svg").c_str())});
    }

    auto view = std::make_unique<DdfxEditorView>(std::move(svg), std::move(controls),
                                                 std::move(radio), std::move(hovers),
                                                 std::move(hits), std::move(presets),
                                                 std::move(tags), std::move(fx_types));
    // DesignFrameView rotates each needle on drag from its own element value, so
    // the knobs turn without any host binding. APVTS param binding (per-knob
    // gesture callbacks → store) is the next step, once the knobs track cleanly.
    view->set_requires_gpu_host(true);
    return view;
#else
    return nullptr;
#endif
}

} // namespace knobpg
