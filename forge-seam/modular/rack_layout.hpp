#pragma once

// Where the panels and cables go.
//
// Split from the painting deliberately. A rack preview is mostly arithmetic --
// panels butt together at their true widths, jacks sit at captured coordinates,
// cables hang between them -- and arithmetic can be asserted exactly, while a
// painted frame can only be eyeballed. Everything here is pure: no view, no
// canvas, no clock.
//
// A patch that draws a cable to the wrong jack is worse than one that draws no
// cable, because it looks authoritative. So the awkward case is modelled rather
// than papered over: a module nobody has ever placed in a rack has no jack
// coordinates, and its cables dock at the panel edge instead of guessing a
// position.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace forge_modular {

/// What a signal is for. Determines the cable's colour, which is the colour
/// Rack itself will show -- the role is written into the patch's colour field
/// at generation, so the preview is not a private convention.
enum class SignalRole { audio, pitch, clock, mod };

/// The colour Rack draws for a role, as 0xRRGGBB.
std::uint32_t role_color(SignalRole role);

/// A jack on a panel.
struct Port {
    std::string id;
    std::string name;      ///< what the panel silkscreens
    float x = 0.5f;        ///< across the panel, 0..1 of its width
    float y = 0.0f;        ///< down the panel, in unscaled panel points
    bool input = true;
};

/// One module in the rack.
struct RackModule {
    std::string id;
    std::string brand;
    std::string name;
    int hp = 8;                 ///< Eurorack width; panels butt at true width
    std::vector<Port> ports;
    /// False when this module has never been placed in a rack, so no jack
    /// coordinates were ever captured for it.
    bool placed = true;
    /// False when the module's own panel artwork is unavailable.
    bool has_artwork = true;
    /// How the module is named in prose, when that differs from `name`.
    ///
    /// `name` stays the model slug because the panel artwork is filed under
    /// it. Prose wants "MULT 1" where two mults would otherwise both read
    /// "MULT" -- a cross-modulation patch whose two oscillators share a name
    /// reads as one oscillator modulating itself. Empty means the two agree.
    std::string display;
};

/// A patch cable.
struct Connection {
    std::string from_module, from_port;
    std::string to_module, to_port;
    SignalRole role = SignalRole::audio;
    std::string why;   ///< the reasoning, shown at Standard and Learning
};

/// A laid-out panel.
struct PanelBox {
    std::string id;
    float x = 0, y = 0, width = 0, height = 0;
    bool placed = true;
    bool has_artwork = true;
};

/// A laid-out jack, in view coordinates.
struct JackPoint {
    float x = 0, y = 0;
    std::string name;
    /// True when this point is a dock at the panel edge rather than a real
    /// jack centre: the module was never placed, so its coordinates are
    /// unknown and the cable ends honestly at the edge.
    bool docked = false;
};

/// The whole rack, positioned.
struct RackLayout {
    float scale = 1.0f;
    float origin_x = 0, origin_y = 0;
    float total_width = 0;   ///< unscaled
    std::vector<PanelBox> panels;

    const PanelBox* panel(const std::string& id) const;
};

/// One Eurorack HP, in unscaled points. A 12 HP panel is 12 of these wide.
inline constexpr float kHorizontalPitch = 15.0f;
/// A 3U panel's height, unscaled.
inline constexpr float kPanelHeight = 380.0f;

/// Lay the rack out inside a viewport.
///
/// Panels butt together with no gutters, because that is what a rack does.
/// Widths are fixed, so the strip scales as one and nothing is nudged to fit --
/// a preview that fudges a panel's width to make a row look tidy is lying about
/// the rack the user will get.
RackLayout layout_rack(const std::vector<RackModule>& modules,
                       float viewport_width, float viewport_height);

/// Where a cable ends.
///
/// `other` is the module at the far end: an unplaced module's dock is chosen on
/// the side facing its partner, so cables do not cross back over the panel.
JackPoint port_point(const RackLayout& layout,
                     const std::vector<RackModule>& modules,
                     const std::string& module_id, const std::string& port_id,
                     const std::string& other_module_id);

/// A cable's hanging curve: start, control point, end.
struct CableCurve {
    float x1 = 0, y1 = 0;
    float cx = 0, cy = 0;
    float x2 = 0, y2 = 0;
};

/// The curve for a cable between two points, `t` being how far it has been
/// drawn in (1 for a finished patch). A cable sags under its own weight, and
/// the sag grows with the distance it spans.
CableCurve cable_curve(const JackPoint& from, const JackPoint& to, float t = 1.0f);

/// How many straight pieces a cable is drawn as. Shared by the painting and by
/// the hit test, so the cable a pointer finds is the cable that was drawn --
/// two different flattenings would put the sag in two different places and the
/// glow would land on a cable the pointer is not over.
inline constexpr int kCableSegments = 24;

/// A point along a cable, `t` from 0 at the output jack to 1 at the input.
void cable_point(const CableCurve& c, float t, float& x, float& y);

/// How far a point is from a cable, in the same units the layout is in.
///
/// Measured to the flattened curve rather than to the straight line between
/// the jacks: a cable sags, and the gap between the two is most of a panel's
/// height on a long run -- so a straight-line test lights a cable while the
/// pointer is nowhere near it.
float distance_to_cable(const CableCurve& c, float x, float y);

}  // namespace forge_modular
