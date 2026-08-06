#include "browser_capture_ir.hpp"
#include "browser_capture_limits.hpp"
#include "browser_capture_styles.hpp"
#include "browser_capture_tree.hpp"

#include <pulp/runtime/crypto.hpp>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

namespace pulp::import_design {

namespace fs = std::filesystem;
using pulp::view::DesignIR;
using pulp::view::IRAssetRef;
using pulp::view::IRTokenIdentity;
using pulp::view::NodeRenderMode;

namespace {

std::string string_member(const choc::value::ValueView& object,
                          const char* key) {
    if (!object.isObject() || !object.hasObjectMember(key) ||
        !object[key].isString())
        return {};
    return object[key].toString();
}

double number_member(const choc::value::ValueView& object,
                     const char* key,
                     double fallback = 0.0) {
    if (!object.isObject() || !object.hasObjectMember(key))
        return fallback;
    return object[key].getWithDefault<double>(fallback);
}

bool validate_reference_geometry(double logical_width,
                                 double logical_height,
                                 double dpr,
                                 std::string& error) {
    if (dpr != static_cast<double>(
                   browser_capture::kDefaultDeviceScaleFactor)) {
        error = "browser capture reference requires DPR 2";
        return false;
    }
    const auto valid_logical_dimension = [](double value) {
        return std::isfinite(value) && value > 0.0 &&
               std::trunc(value) == value &&
               value <=
                   browser_capture::kMaximumLogicalViewportDimension;
    };
    if (!valid_logical_dimension(logical_width) ||
        !valid_logical_dimension(logical_height)) {
        error =
            "browser capture reference requires finite positive integer "
            "logical dimensions no larger than 8192";
        return false;
    }
    if (!browser_capture::viewport_within_capture_limits(
            static_cast<int>(logical_width),
            static_cast<int>(logical_height),
            browser_capture::kDefaultDeviceScaleFactor)) {
        error =
            "browser capture reference exceeds the 64 megapixel safety limit";
        return false;
    }
    return true;
}

choc::value::ValueView object_member(const choc::value::ValueView& object,
                                     const char* key) {
    if (!object.isObject() || !object.hasObjectMember(key) ||
        !object[key].isObject())
        return {};
    return object[key];
}

/// object_member insists the member is an OBJECT and hands back an empty view
/// for an array, which reads as "absent" — a silent zero rather than an error.
choc::value::ValueView array_member(const choc::value::ValueView& object,
                                    const char* key) {
    if (!object.isObject() || !object.hasObjectMember(key) ||
        !object[key].isArray())
        return {};
    return object[key];
}

bool load_token_report(const fs::path& path,
                       DesignIR& ir,
                       int expected_colors,
                       int expected_dimensions,
                       int expected_strings,
                       std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "could not open browser token report: " + path.string();
        return false;
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    choc::value::Value report;
    try {
        report = choc::json::parse(bytes.str());
    } catch (const std::exception& e) {
        error = std::string("invalid browser token JSON: ") + e.what();
        return false;
    }
    if (!report.isObject() ||
        string_member(report, "schema") != "pulp-browser-tokens-v1" ||
        number_member(report, "version", 0.0) != 1.0) {
        error =
            "unsupported browser token report (expected pulp-browser-tokens-v1 version 1)";
        return false;
    }

    const auto colors = object_member(report, "colors");
    for (uint32_t i = 0; i < colors.size(); ++i) {
        const auto member = colors.getObjectMemberAt(i);
        if (member.value.isString())
            ir.tokens.colors[std::string(member.name)] =
                member.value.toString();
    }
    const auto dimensions = object_member(report, "dimensions");
    for (uint32_t i = 0; i < dimensions.size(); ++i) {
        const auto member = dimensions.getObjectMemberAt(i);
        if (member.value.isFloat() || member.value.isInt())
            ir.tokens.dimensions[std::string(member.name)] =
                static_cast<float>(member.value.getWithDefault<double>(0.0));
    }
    const auto strings = object_member(report, "strings");
    for (uint32_t i = 0; i < strings.size(); ++i) {
        const auto member = strings.getObjectMemberAt(i);
        if (member.value.isString())
            ir.tokens.strings[std::string(member.name)] =
                member.value.toString();
    }
    if (expected_colors < 0 || expected_dimensions < 0 ||
        expected_strings < 0 ||
        static_cast<int>(ir.tokens.colors.size()) != expected_colors ||
        static_cast<int>(ir.tokens.dimensions.size()) != expected_dimensions ||
        static_cast<int>(ir.tokens.strings.size()) != expected_strings) {
        error = "browser token counts do not match capture envelope";
        return false;
    }
    const auto identities = object_member(report, "source_identity");
    for (uint32_t i = 0; i < identities.size(); ++i) {
        const auto member = identities.getObjectMemberAt(i);
        if (!member.value.isObject()) continue;
        IRTokenIdentity identity;
        identity.source_id = string_member(member.value, "source_id");
        identity.source_collection =
            string_member(member.value, "source_collection");
        identity.source_mode = string_member(member.value, "source_mode");
        identity.source_adapter =
            string_member(member.value, "source_adapter");
        ir.tokens.source_identity[std::string(member.name)] =
            std::move(identity);
    }
    return true;
}

bool validate_semantic_report(const fs::path& path,
                              int expected_candidates,
                              int expected_resolved,
                              int expected_unresolved,
                              std::string& error) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    choc::value::Value report;
    try {
        report = choc::json::parse(bytes.str());
    } catch (const std::exception& e) {
        error = std::string("invalid browser semantic JSON: ") + e.what();
        return false;
    }
    const auto summary = object_member(report, "summary");
    if (!report.isObject() ||
        string_member(report, "schema") != "pulp-browser-semantics-v1" ||
        number_member(report, "version", 0.0) != 1.0 ||
        !summary.isObject()) {
        error =
            "unsupported browser semantic report "
            "(expected pulp-browser-semantics-v1 version 1 with summary)";
        return false;
    }
    const auto candidates =
        static_cast<int>(number_member(summary, "candidates", -1.0));
    const auto resolved =
        static_cast<int>(number_member(summary, "resolved", -1.0));
    const auto unresolved =
        static_cast<int>(number_member(summary, "unresolved", -1.0));
    if (candidates < 0 || resolved < 0 || unresolved < 0 ||
        candidates != resolved + unresolved ||
        candidates != expected_candidates || resolved != expected_resolved ||
        unresolved != expected_unresolved) {
        error = "browser semantic summary does not match capture envelope";
        return false;
    }
    return true;
}

/// A declared pointer, expressed the way the renderer wants it.
///
/// `Knob::paint` draws the design's own pointer as a radial stroke swept along
/// the value arc, and takes its extent as FRACTIONS of the dial's half-extent
/// (the same convention `hoist_captured_art_knobs` records for the Figma lane)
/// so the numbers survive any later rescale of the control.
///
/// The declared rectangle is not axis-aligned with the radius it sits on: a dot
/// at 7 o'clock is a square whose diagonal, not its width, spans the radial
/// direction. Projecting the box's half-extents onto the radial unit vector and
/// its perpendicular gives the true along-radius reach and across-radius width
/// for a pointer at ANY angle, which a plain width/height read does not.
struct DeclaredPointer {
    float r_in = 0.0f;
    float r_out = 0.0f;
    float width = 0.0f;
};

/// The pointer's box, as the capture recorded it.
///
/// `left`/`top`/`width`/`height` are the painted footprint in page CSS px --
/// what `getBoundingClientRect` measures, plus any stroke the capture could
/// recover. That box is axis-aligned with the SCREEN, so for a rotated pointer
/// it is the box the shape's diagonal sweeps rather than the shape: a 4x38
/// needle at 38 degrees measures 26.5x32.4, and a width read off it is ten times
/// the truth.
///
/// `intrinsic_*` and the matrix are the same pointer described in its OWN
/// coordinate space, which is the only description a rotation does not distort.
/// `oriented` says whether the capture supplied them; a report that predates
/// them, or an element that could answer neither of the two APIs, leaves it
/// false and the footprint is all there is.
struct IndicatorBox {
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool oriented = false;
    double intrinsic_width = 0.0;
    double intrinsic_height = 0.0;
    // The intrinsic box centre transformed into page coordinates. An
    // asymmetric SVG shape need not paint pixels symmetrically inside its
    // geometry box, so its client-rect centre is only an erasure hint.
    double center_x = 0.0;
    double center_y = 0.0;
    // The linear part of element space -> page, column-major as CSS writes it:
    // local x maps to (a, b), local y to (c, d).
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
};

std::optional<DeclaredPointer> pointer_fractions(double dial_left,
                                                 double dial_top,
                                                 double dial_width,
                                                 double dial_height,
                                                 const IndicatorBox& ind) {
    const double half = std::min(dial_width, dial_height) * 0.5;
    if (!(half > 0.0)) return std::nullopt;
    // The pointer's two half-extent VECTORS in page coordinates.
    //
    // Without an element space to read, these are the footprint's own axes and
    // the projection below is a support function of an axis-aligned box, which
    // is what this has always computed. With one, they are the images of the
    // element's own axes -- identical for an unrotated pointer, and the whole
    // fix for a rotated one, because the element's box is the shape and the
    // footprint is only the box the shape sweeps.
    double xx = ind.width * 0.5;
    double xy = 0.0;
    double yx = 0.0;
    double yy = ind.height * 0.5;
    if (ind.oriented) {
        xx = ind.a * ind.intrinsic_width * 0.5;
        xy = ind.b * ind.intrinsic_width * 0.5;
        yx = ind.c * ind.intrinsic_height * 0.5;
        yy = ind.d * ind.intrinsic_height * 0.5;
    }
    // Reject a box with NO extent on EITHER axis -- that carries no direction
    // to sweep along. A box with one zero axis is a different thing and must
    // survive: an SVG <line> or <path> drawn straight up, down, left or right
    // reports zero extent across its own axis, because a client rect excludes
    // stroke. The capture recovers the painted width from the stroke where it
    // can, but where it cannot -- a shape with no stroke to read -- the right
    // answer is a correctly PLACED pointer of defaulted thickness, not a
    // dropped one. `&&` rather than `||` is the whole difference.
    //
    // Measured on the axes actually used, not on the footprint. A rotated line
    // is zero-width in its own space and fat in its footprint, so a guard left
    // behind on the footprint would pass exactly the box it should refuse.
    //
    // This predicate exists twice, once here and once as the capture's own
    // guard in browser_capture/semantics.mjs. They are in different languages,
    // so neither grep finds the other; relaxing one alone leaves the other
    // refusing the same box, one layer down and just as silently.
    const double x_extent = std::hypot(xx, xy);
    const double y_extent = std::hypot(yx, yy);
    if (!(x_extent > 0.0) && !(y_extent > 0.0)) return std::nullopt;
    const double cx = dial_left + dial_width * 0.5;
    const double cy = dial_top + dial_height * 0.5;
    // New captures carry the transformed intrinsic-box centre. The footprint
    // centre remains the compatibility fallback: it is exact for rectangular
    // HTML pointers and lines, but an asymmetric SVG path may paint unevenly
    // inside the geometry box and therefore produce an off-centre client rect.
    const double pointer_cx = ind.oriented
                                  ? ind.center_x
                                  : ind.left + ind.width * 0.5;
    const double pointer_cy = ind.oriented
                                  ? ind.center_y
                                  : ind.top + ind.height * 0.5;
    const double dx = pointer_cx - cx;
    const double dy = pointer_cy - cy;
    const double distance = std::sqrt(dx * dx + dy * dy);
    // A pointer centred on the dial has no radial direction to sweep along, so
    // there is nothing to reproduce. Refuse rather than divide by zero and
    // stamp a pointer that pivots on itself.
    if (!(distance > 0.0)) return std::nullopt;
    const double ux = dx / distance;
    const double uy = dy / distance;
    // Support function of the box along the radial axis, and along the axis
    // perpendicular to it.
    const double along =
        std::abs(xx * ux + xy * uy) + std::abs(yx * ux + yy * uy);
    const double across =
        std::abs(xy * ux - xx * uy) + std::abs(yy * ux - yx * uy);
    if (!std::isfinite(along) || !std::isfinite(across)) return std::nullopt;
    DeclaredPointer out;
    out.r_in = static_cast<float>(std::max(0.0, distance - along) / half);
    out.r_out = static_cast<float>((distance + along) / half);
    out.width = static_cast<float>((2.0 * across) / half);
    if (!(out.r_out > out.r_in)) return std::nullopt;
    return out;
}

/// A browser-resolved colour as `#rrggbb` / `#rrggbbaa`.
///
/// `getComputedStyle` always answers in `rgb()` / `rgba()` form, and the two
/// consumers of `knob_ind_color` do not agree on what they can read: the native
/// materializer parses any CSS colour, the scripted bridge parses hex only and
/// silently falls back to near-white. Normalizing here means the design's
/// pointer colour survives BOTH paths instead of only the one that happens to
/// be exercised. Anything else — an author-declared `oklch()`, a named colour —
/// is carried through verbatim for the parser that can read it.
std::string css_color_to_hex(const std::string& value) {
    const auto open = value.find('(');
    if (open == std::string::npos ||
        (value.compare(0, 4, "rgb(") != 0 && value.compare(0, 5, "rgba(") != 0))
        return value;
    double channels[4] = {0.0, 0.0, 0.0, 1.0};
    int count = 0;
    std::size_t cursor = open + 1;
    while (count < 4 && cursor < value.size()) {
        try {
            std::size_t consumed = 0;
            channels[count] = std::stod(value.substr(cursor), &consumed);
            if (consumed == 0) break;
            cursor += consumed;
            ++count;
        } catch (const std::exception&) {
            break;
        }
        while (cursor < value.size() &&
               (value[cursor] == ',' || value[cursor] == ' ' ||
                value[cursor] == '/'))
            ++cursor;
    }
    if (count < 3) return value;
    const auto byte = [](double v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.0, 255.0)));
    };
    char buffer[10];
    if (count == 4 && channels[3] < 0.999) {
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x",
                      byte(channels[0]), byte(channels[1]), byte(channels[2]),
                      byte(channels[3] * 255.0));
    } else {
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x",
                      byte(channels[0]), byte(channels[1]), byte(channels[2]));
    }
    return buffer;
}

/// A CSS-pixel page rectangle as the capture PNG's own integer pixel rectangle,
/// serialized "x,y,w,h".
///
/// Rounded at the edges rather than at origin+size so a crop and the pointer
/// inside it stay aligned: rounding a size independently of its origin can move
/// a boundary by a pixel, which is enough to leave a sliver of the erased
/// pointer showing at the edge of the rectangle meant to contain it.
std::string device_pixel_rect(double left, double top,
                              double width, double height, double dpr) {
    const long x0 = std::lround(left * dpr);
    const long y0 = std::lround(top * dpr);
    const long x1 = std::lround((left + width) * dpr);
    const long y1 = std::lround((top + height) * dpr);
    return std::to_string(x0) + "," + std::to_string(y0) + "," +
           std::to_string(x1 - x0) + "," + std::to_string(y1 - y0);
}

/// Lower each bound semantic candidate into a control node beneath the
/// faithful-capture backdrop.
///
/// The capture is a picture; these children are what make it a panel. Every
/// step downstream already exists — design_codegen emits createKnob/createFader
/// /createMeter for any node carrying audio_widget, and the designed-control
/// skin paints value-only geometry so the design underneath is not erased.
///
/// Geometry comes from the author-declared paint box when present. It is NOT
/// the component box: a knob's component includes its caption (116x139.9 where
/// the dial is 116x116), so painting a value ring into it lands the ring ~12px
/// below the dial centre — which renders, looks almost right, and is invisible
/// to pixel-parity and to the component and macro contracts alike.
///
/// Only BOUND candidates become controls. An unbound candidate drives nothing,
/// and a live widget over the design that moves no parameter is worse than
/// leaving that part of the picture alone.
int lower_semantic_controls(const fs::path& path,
                            const DesignIR& ir,
                            double dx, double dy, double dpr,
                            pulp::view::IRNode& root,
                            const CapturedStyleIndex* styles,
                            bool body_is_native_underlay,
                            int& styled_controls,
                            int& undeclared_paint_boxes,
                            std::string& error) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    choc::value::Value report;
    try {
        report = choc::json::parse(bytes.str());
    } catch (const std::exception& e) {
        error = std::string("invalid browser semantic JSON: ") + e.what();
        return -1;
    }
    const auto candidates = array_member(report, "candidates");
    if (!candidates.isArray()) return 0;

    int lowered = 0;
    for (uint32_t i = 0; i < candidates.size(); ++i) {
        const auto candidate = candidates[i];
        if (!candidate.isObject()) continue;
        if (string_member(candidate, "binding_status") != "bound") continue;

        const auto kind = string_member(candidate, "kind");
        auto widget = pulp::view::AudioWidgetType::none;
        if (kind == "knob") widget = pulp::view::AudioWidgetType::knob;
        else if (kind == "fader") widget = pulp::view::AudioWidgetType::fader;
        else if (kind == "meter") widget = pulp::view::AudioWidgetType::meter;
        else continue;  // buttons and unknowns stay part of the backdrop

        const auto data = object_member(candidate, "data_pulp");
        // Which attribute supplied the key IS the declared role: a control
        // DRIVES its parameter, a meter only DISPLAYS one. Keying the role off
        // the attribute rather than the widget kind keeps this agreeing by
        // construction with the authoring contract an importing host checks
        // before the browser runs -- a fader beside its own level meter binds
        // the same parameter twice, which is completely ordinary and must not
        // read as two controls driving one parameter.
        std::string param = string_member(data, "param");
        bool displays_only = false;
        if (param.empty()) {
            param = string_member(data, "meter");
            displays_only = !param.empty();
        }
        if (param.empty()) continue;  // "bound" without a key is not a binding

        // Prefer the declared paint box; fall back to the component box and
        // count it, so an undeclared control is reported rather than silently
        // mispainted. A meter legitimately has no inner paint box — the
        // component IS its track — so the fallback is correct there.
        auto box = object_member(candidate, "paint_bounds");
        if (!box.isObject()) {
            box = object_member(candidate, "bounds");
            ++undeclared_paint_boxes;
        }

        pulp::view::IRNode control;
        control.type = "frame";
        control.name = string_member(candidate, "name");
        control.audio_widget = widget;
        // design_codegen keys the host binding off attributes["binding"] —
        // IRNode::param_key does not exist; that field belongs to the
        // geometry-detected element struct, which is a different lane.
        // Two consumers read the host binding under DIFFERENT names, and a
        // control that carries only one is silently half-wired: the JS codegen
        // keys off "binding", while the C++ path's binding metadata reads
        // "pulpParamKey". Exporting a panel with only the first produced eight
        // real widgets and an EMPTY binding manifest -- knobs that render and
        // move nothing.
        control.attributes["binding"] = param;
        // The JS emitter reads one key for both roles and branches on the
        // widget type (bindMeter vs bindWidgetToParam), so "binding" is
        // shared. The C++ / manifest vocabulary separates them, and writing a
        // display under pulpParamKey claims the parameter is DRIVEN here --
        // which reads downstream as a second control on a parameter that
        // already has one.
        control.attributes[displays_only ? "pulpMeterValueKey" : "pulpParamKey"] =
            param;
        // A param key alone gets the control into the binding MANIFEST but not
        // into the emitted C++. `collect_resolved_binding_plan` admits a helper
        // route only when the node ALSO carries a route id and a stable anchor:
        // the emitted helper finds its widget by anchor and claims it by route
        // id, so without both there is nothing to find and nothing to claim.
        // The manifest then lists bindings that no generated code applies --
        // which reads as wired and moves nothing.
        //
        // Keyed on the macro plus the candidate index rather than the macro
        // alone: a meter may legitimately share a macro with the control that
        // drives it, and two routes with one anchor make the emitted lookup
        // ambiguous, which the generated code treats as no match at all.
        const auto anchor = "capture:" + param + ":" + std::to_string(i);
        control.stable_anchor_id = anchor;
        control.anchor_strategy = "path";
        control.attributes["pulpRouteId"] = anchor;
        // The body of this control is the layer beneath it, so it carries no
        // background of its own and would otherwise fail the has-a-body test
        // that selects the value-only skin -- and be painted over by an opaque
        // default widget body. Stated explicitly per mode: on a natively drawn
        // panel the rule can no longer be inferred from a bitmap's existence,
        // because there is no bitmap.
        control.attributes["designed_body"] =
            body_is_native_underlay ? "underlay" : "capture";
        // The caption is already in the capture. audio_label would draw a
        // second copy on top of it; the name survives on the node for host
        // parameter naming.
        control.audio_label.clear();
        // The design draws the control at a value; the widget must open at the
        // same one or the panel changes the moment it loads. Declared, not
        // inferred: the value lives in a CSS custom property the stylesheet
        // reads, and which property that is differs per design system.
        const auto declared_value = string_member(data, "value");
        if (!declared_value.empty()) {
            try {
                control.audio_default = std::stof(declared_value);
            } catch (const std::exception&) {
                // A malformed value must not take the control down with it;
                // the default stands and the design still renders.
            }
        }
        control.style.position = "absolute";
        control.style.left =
            static_cast<float>(number_member(box, "left", 0.0) + dx);
        control.style.top =
            static_cast<float>(number_member(box, "top", 0.0) + dy);
        control.style.width = static_cast<float>(number_member(box, "width", 0.0));
        control.style.height = static_cast<float>(number_member(box, "height", 0.0));
        // Chrome already solved this element's appearance; record it so the
        // node carries the gradient, radius, and shadow stack that make it look
        // like the designed control instead of only a transparent hit box.
        // Applied after the geometry above and confined to appearance
        // properties, so the design's paint box stays the placement authority.
        if (styles != nullptr) {
            const auto backend_node_id = static_cast<int>(
                number_member(candidate, "backend_node_id", -1.0));
            if (backend_node_id >= 0) {
                CapturedBox paint_box;
                paint_box.left = number_member(box, "left", 0.0);
                paint_box.top = number_member(box, "top", 0.0);
                paint_box.width = number_member(box, "width", 0.0);
                paint_box.height = number_member(box, "height", 0.0);
                const auto computed =
                    styles->styles_for(backend_node_id, paint_box);
                if (!computed.empty()) {
                    apply_computed_styles(computed, paint_box, control.style);
                    ++styled_controls;
                }
            }
        }
        // The value layer's colours come from the DESIGN's tokens, not the
        // widget defaults. DesignedControlSkin's header says exactly this, but
        // its call site passes a default-constructed skin whose accent is a
        // hardcoded teal -- so a warm cream panel drew teal arcs and a green
        // meter while the browser capture beside it was perfectly coherent.
        const auto token = [&ir](const char* name) -> std::string {
            const auto it = ir.tokens.colors.find(name);
            return it == ir.tokens.colors.end() ? std::string{} : it->second;
        };
        // The control's OWN resolved accent wins over the pack token. A panel
        // that scopes or overrides its palette -- which a good one does -- is
        // invisible to the pack's token set, so the pack accent lands a green
        // arc on an orange knob and our render comes out WORSE than the
        // browser's, with every pixel gate still green because the arc is ours.
        //
        // Read from the CANDIDATE, not from `data`: `data` is the author's
        // data-pulp-* attributes, and the accent is resolved by the browser
        // rather than declared. Reading it from `data` compiles, runs, and is
        // always empty -- a silent fall-through to the pack token.
        const auto declared_accent = string_member(candidate, "accent");
        if (!declared_accent.empty())
            control.attributes["design_accent"] = declared_accent;
        else if (const auto accent = token("css/accent"); !accent.empty())
            control.attributes["design_accent"] = accent;
        if (const auto track = token("css/line-strong"); !track.empty())
            control.attributes["design_track"] = track;
        if (const auto ind = token("css/text-strong"); !ind.empty())
            control.attributes["design_indicator"] = ind;

        // The design's OWN pointer, when it declared one.
        //
        // Recorded in the SAME vocabulary the Figma lane already emits
        // (`hoist_captured_art_knobs` -> knob_ind_r_in / _r_out / _w /
        // _color), as fractions of the paint box's HALF-extent, so the
        // consumer needs no second set of names and no second code path. A
        // design that declares nothing keeps the widget's derived tick, which
        // is the common case and not a failure.
        //
        // `design_indicator` above is only a COLOUR -- with no geometry the
        // engine has nothing to move, so an imported knob showed the pointer
        // frozen wherever the capture happened to catch it.
        //
        // The two device-pixel rectangles are hand-off state for the sprite
        // pass (`apply_browser_capture_knob_sprites`), which crops the control
        // out of the panel capture and erases the pointer baked into that crop.
        // It consumes and removes them; they are not a runtime contract. Only a
        // knob carries them: they exist to feed that pass, which skins dials.
        const auto ind_box = object_member(candidate, "indicator_bounds");
        if (ind_box.isObject() && box.isObject()) {
            const double dial_left = number_member(box, "left", 0.0);
            const double dial_top = number_member(box, "top", 0.0);
            const double dial_w = number_member(box, "width", 0.0);
            const double dial_h = number_member(box, "height", 0.0);
            IndicatorBox ind;
            ind.left = number_member(ind_box, "left", 0.0);
            ind.top = number_member(ind_box, "top", 0.0);
            ind.width = number_member(ind_box, "width", 0.0);
            ind.height = number_member(ind_box, "height", 0.0);
            // The pointer in its OWN space, when the capture could describe it
            // there. Both halves or neither: an element size without the matrix
            // that scales it reads SVG user units as CSS px, which is the
            // viewBox error one layer along, and a matrix without a size has
            // nothing to scale.
            const auto intrinsic = object_member(ind_box, "intrinsic");
            const auto matrix = array_member(ind_box, "transform");
            if (intrinsic.isObject() && matrix.size() >= 6) {
                const auto component = [&](uint32_t index) {
                    return matrix[index].getWithDefault<double>(
                        std::numeric_limits<double>::quiet_NaN());
                };
                const double a = component(0);
                const double b = component(1);
                const double c = component(2);
                const double d = component(3);
                const double e = component(4);
                const double f = component(5);
                const double ix = number_member(intrinsic, "x", 0.0);
                const double iy = number_member(intrinsic, "y", 0.0);
                const double iw = number_member(intrinsic, "width", -1.0);
                const double ih = number_member(intrinsic, "height", -1.0);
                if (std::isfinite(a) && std::isfinite(b) && std::isfinite(c) &&
                    std::isfinite(d) && std::isfinite(e) && std::isfinite(f) &&
                    std::isfinite(ix) && std::isfinite(iy) && iw >= 0.0 &&
                    ih >= 0.0) {
                    ind.oriented = true;
                    ind.intrinsic_width = iw;
                    ind.intrinsic_height = ih;
                    ind.a = a;
                    ind.b = b;
                    ind.c = c;
                    ind.d = d;
                    const double local_cx = ix + iw * 0.5;
                    const double local_cy = iy + ih * 0.5;
                    ind.center_x = a * local_cx + c * local_cy + e;
                    ind.center_y = b * local_cx + d * local_cy + f;
                }
            }
            if (const auto pointer = pointer_fractions(
                    dial_left, dial_top, dial_w, dial_h, ind)) {
                control.attributes["knob_ind_r_in"] =
                    std::to_string(pointer->r_in);
                control.attributes["knob_ind_r_out"] =
                    std::to_string(pointer->r_out);
                control.attributes["knob_ind_w"] =
                    std::to_string(pointer->width);
                if (const auto color =
                        string_member(candidate, "indicator_color");
                    !color.empty())
                    control.attributes["knob_ind_color"] = css_color_to_hex(color);
                if (widget == pulp::view::AudioWidgetType::knob) {
                    control.attributes["browser_sprite_crop_px"] =
                        device_pixel_rect(dial_left, dial_top, dial_w, dial_h, dpr);
                    // The PAINTED footprint, deliberately: this pass crops the
                    // control out of the panel capture and erases the pointer
                    // baked into that crop, so it wants every pixel the pointer
                    // covers. For a rotated needle that is the fat box -- the
                    // one thing the geometry above must not use and this must.
                    control.attributes["browser_sprite_indicator_px"] =
                        device_pixel_rect(ind.left, ind.top, ind.width,
                                          ind.height, dpr);
                }
            }
        }

        // Sit above the lowered decoration, explicitly.
        //
        // Lowering assigns every composed node a z-index from Chrome's paint
        // order, and these synthesized controls are appended afterwards without
        // one — so they defaulted to `auto` and sat BENEATH any decoration
        // carrying an explicit z-index. Hit-testing follows paint order, so a
        // glow or gradient band drawn over a knob answered the press meant for
        // it: the panel rendered correctly and responded to nothing. Appending
        // last does not save them, because z-index beats document order.
        //
        // Root children are siblings in one stacking context, so a z-index above
        // every root sibling puts the control above those siblings' whole
        // subtrees — which is where the covering bands live. Computed from the
        // tree rather than a constant: a fixed number silently stops working the
        // day a design nests one level deeper than it did today.
        int max_sibling_z = 0;
        for (const auto& sibling : root.children)
            if (sibling.style.z_index)
                max_sibling_z = std::max(max_sibling_z, *sibling.style.z_index);
        control.style.z_index = max_sibling_z + 1;

        root.children.push_back(std::move(control));
        ++lowered;
    }
    return lowered;
}

bool validate_png_header(const fs::path& path,
                         int expected_width,
                         int expected_height,
                         std::string& error) {
    std::ifstream input(path, std::ios::binary);
    unsigned char header[24]{};
    input.read(reinterpret_cast<char*>(header), sizeof(header));
    constexpr unsigned char signature[] =
        {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (input.gcount() != static_cast<std::streamsize>(sizeof(header)) ||
        !std::equal(std::begin(signature), std::end(signature), header)) {
        error = "browser reference is not a PNG";
        return false;
    }
    auto be32 = [&](int offset) {
        return (static_cast<int>(header[offset]) << 24) |
               (static_cast<int>(header[offset + 1]) << 16) |
               (static_cast<int>(header[offset + 2]) << 8) |
               static_cast<int>(header[offset + 3]);
    };
    if (be32(16) != expected_width || be32(20) != expected_height) {
        error = "browser reference PNG dimensions do not match capture envelope";
        return false;
    }
    return true;
}

std::string file_sha256(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    return pulp::runtime::sha256_hex(bytes.data(), bytes.size());
}

bool is_sha256(std::string_view value) {
    return value.size() == 64 &&
           std::all_of(
               value.begin(), value.end(), [](unsigned char character) {
                   return (character >= '0' && character <= '9') ||
                          (character >= 'a' && character <= 'f');
               });
}

bool integer_member(const choc::value::ValueView& object,
                    const char* key,
                    int minimum,
                    int maximum,
                    int& value) {
    if (!object.isObject() || !object.hasObjectMember(key) ||
        (!object[key].isInt() && !object[key].isFloat())) {
        return false;
    }
    const auto number = object[key].getWithDefault<double>(
        std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(number) || std::trunc(number) != number ||
        number < minimum || number > maximum) {
        return false;
    }
    value = static_cast<int>(number);
    return true;
}

bool validate_interaction_report(
    const fs::path& path,
    const choc::value::ValueView& provenance,
    std::string& plan_sha256,
    std::string& report_sha256,
    int& action_count,
    std::string& error) {
    if (string_member(provenance, "schema") !=
            "pulp-browser-interactions-v1" ||
        number_member(provenance, "version", 0.0) != 1.0) {
        error =
            "unsupported browser interaction provenance "
            "(expected pulp-browser-interactions-v1 version 1)";
        return false;
    }
    plan_sha256 = string_member(provenance, "plan_sha256");
    report_sha256 = string_member(provenance, "report_sha256");
    if (!is_sha256(plan_sha256) || !is_sha256(report_sha256)) {
        error =
            "browser interaction provenance requires lowercase SHA-256 "
            "plan and report hashes";
        return false;
    }
    if (!integer_member(provenance, "action_count", 1, 32, action_count)) {
        error =
            "browser interaction provenance requires an action_count from 1 to 32";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "could not open browser interaction report: " + path.string();
        return false;
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    const auto report_bytes = bytes.str();
    if (pulp::runtime::sha256_hex(report_bytes) != report_sha256) {
        error =
            "browser interaction report hash does not match capture envelope";
        return false;
    }
    choc::value::Value report;
    try {
        report = choc::json::parse(report_bytes);
    } catch (const std::exception& exception) {
        error = std::string("invalid browser interaction JSON: ") +
                exception.what();
        return false;
    }
    int report_action_count = 0;
    if (!report.isObject() ||
        string_member(report, "schema") !=
            "pulp-browser-interactions-v1" ||
        number_member(report, "version", 0.0) != 1.0 ||
        string_member(report, "plan_sha256") != plan_sha256 ||
        !integer_member(
            report, "action_count", 1, 32, report_action_count) ||
        !report.hasObjectMember("actions") ||
        !report["actions"].isArray() ||
        report["actions"].size() !=
            static_cast<uint32_t>(report_action_count) ||
        report_action_count != action_count) {
        error =
            "browser interaction report does not match capture provenance";
        return false;
    }
    const auto actions = report["actions"];
    for (uint32_t index = 0; index < actions.size(); ++index) {
        const auto action = actions[static_cast<int>(index)];
        const auto name = string_member(action, "action");
        const bool allowed =
            name == "click" || name == "type" || name == "wait-for" ||
            name == "wait-ms";
        if (!action.isObject() || !allowed ||
            string_member(action, "status") != "completed" ||
            action.hasObjectMember("text") ||
            action.hasObjectMember("text_sha256")) {
            error =
                "browser interaction report contains invalid or private action evidence";
            return false;
        }
    }
    return true;
}

std::optional<fs::path> contained_sidecar(const fs::path& envelope,
                                          std::string_view authored,
                                          std::string& error) {
    if (authored.empty()) {
        error = "capture envelope names an empty sidecar path";
        return std::nullopt;
    }
    const fs::path relative{authored};
    if (relative.is_absolute()) {
        error = "capture envelope sidecar path must be relative: " +
                relative.string();
        return std::nullopt;
    }

    std::error_code ec;
    const auto base = fs::weakly_canonical(envelope.parent_path(), ec);
    if (ec) {
        error = "could not resolve capture directory: " + ec.message();
        return std::nullopt;
    }
    const auto candidate = fs::weakly_canonical(base / relative, ec);
    if (ec) {
        error = "could not resolve capture sidecar '" + relative.string() +
                "': " + ec.message();
        return std::nullopt;
    }
    const auto rel = fs::relative(candidate, base, ec);
    if (ec || rel.empty() || rel.is_absolute() ||
        *rel.begin() == fs::path("..")) {
        error = "capture sidecar escapes the capture directory: " +
                relative.string();
        return std::nullopt;
    }
    if (!fs::is_regular_file(candidate, ec) || ec) {
        error = "capture sidecar is missing or not a regular file: " +
                candidate.string();
        return std::nullopt;
    }
    return candidate;
}

}  // namespace

BrowserCaptureIrResult lower_browser_capture_to_ir(
    const fs::path& envelope_path,
    const BrowserCaptureIrOptions& options) {
    BrowserCaptureIrResult result;

    std::ifstream input(envelope_path, std::ios::binary);
    if (!input) {
        result.error = "could not open browser capture envelope: " +
                       envelope_path.string();
        return result;
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();

    choc::value::Value envelope;
    try {
        envelope = choc::json::parse(bytes.str());
    } catch (const std::exception& e) {
        result.error = std::string("invalid browser capture JSON: ") + e.what();
        return result;
    }
    if (!envelope.isObject() ||
        string_member(envelope, "schema") != "pulp-browser-capture-v1" ||
        number_member(envelope, "version", 0.0) != 1.0) {
        result.error =
            "unsupported browser capture envelope (expected pulp-browser-capture-v1 version 1)";
        return result;
    }
    const auto provenance = object_member(envelope, "provenance");
    if (string_member(provenance, "capture_method") != "chromium-cdp" ||
        !envelope.hasObjectMember("documents") ||
        !envelope["documents"].isArray() || envelope["documents"].size() == 0 ||
        !envelope.hasObjectMember("states") ||
        !envelope["states"].isArray() || envelope["states"].size() == 0) {
        result.error =
            "browser capture envelope is missing required provenance, "
            "documents, or states";
        return result;
    }

    const auto reference = object_member(envelope, "reference");
    const std::string reference_id = string_member(reference, "asset_id");
    const std::string reference_path = string_member(reference, "path");
    const double logical_width = number_member(reference, "logical_width");
    const double logical_height = number_member(reference, "logical_height");
    const double dpr = number_member(reference, "device_scale_factor");
    // Where the authored frame sits INSIDE the captured image, in CSS px. The
    // capture is deliberately larger than the design -- the root carries its
    // own padding and the harness grows the extent so drop shadows and
    // absolutely positioned decoration survive -- so the two pictures are not
    // in correspondence until one is cropped to this rect.
    //
    // The member is null on a capture that could not resolve the frame, and
    // null is not a zero offset: object_member hands back an empty view for a
    // null, which reads as absent, and absent must reach the consumer's refusal
    // path rather than becoming an origin of (0,0).
    const auto authored_frame = object_member(reference, "authored_frame");
    const double authored_frame_width =
        number_member(authored_frame, "width", 0.0);
    const double authored_frame_height =
        number_member(authored_frame, "height", 0.0);
    const bool has_authored_frame =
        std::isfinite(authored_frame_width) &&
        std::isfinite(authored_frame_height) &&
        authored_frame_width > 0.0 && authored_frame_height > 0.0;
    // The panel's own bounds, which the capture already measures. The document
    // is the VIEWPORT plus whatever room the overhang needed, so using it as
    // the root opens a plugin far larger than its design with dead space
    // around it.
    const auto surface = object_member(
        object_member(object_member(object_member(envelope, "provenance"),
                                    "viewport"), "document"),
        "primary_surface");
    const double surface_left = number_member(surface, "left", 0.0);
    const double surface_top = number_member(surface, "top", 0.0);
    const double surface_width = number_member(surface, "width", 0.0);
    const double surface_height = number_member(surface, "height", 0.0);
    if (reference_id.empty()) {
        result.error =
            "browser capture reference requires a non-empty asset_id";
        return result;
    }
    if (!validate_reference_geometry(
            logical_width, logical_height, dpr, result.error))
        return result;

    const auto documents = envelope["documents"];
    for (uint32_t i = 0; i < documents.size(); ++i) {
        const auto document = documents[static_cast<int>(i)];
        if (!document.isObject() || string_member(document, "id").empty()) {
            result.error =
                "browser capture document requires a non-empty id";
            return result;
        }
    }
    const auto states = envelope["states"];
    for (uint32_t i = 0; i < states.size(); ++i) {
        const auto state = states[static_cast<int>(i)];
        if (!state.isObject() || string_member(state, "name").empty() ||
            string_member(state, "reference_asset_id") != reference_id) {
            result.error =
                "browser capture state does not reference the canonical asset";
            return result;
        }
    }
    auto reference_png =
        contained_sidecar(envelope_path, reference_path, result.error);
    if (!reference_png) return result;

    const auto semantics = object_member(envelope, "semantics");
    const std::string semantic_path = string_member(semantics, "report");
    auto semantic_report =
        contained_sidecar(envelope_path, semantic_path, result.error);
    if (!semantic_report) return result;
    const int candidate_count =
        static_cast<int>(number_member(semantics, "candidate_count", -1.0));
    const int resolved_count =
        static_cast<int>(number_member(semantics, "resolved_count", -1.0));
    const int unresolved_count =
        static_cast<int>(number_member(semantics, "unresolved_count", -1.0));
    if (!validate_semantic_report(
            *semantic_report, candidate_count, resolved_count,
            unresolved_count, result.error))
        return result;
    const auto tokens = object_member(envelope, "tokens");
    const std::string token_path = string_member(tokens, "report");
    auto token_report =
        contained_sidecar(envelope_path, token_path, result.error);
    if (!token_report) return result;

    std::optional<fs::path> interaction_report;
    std::string interaction_plan_sha256;
    std::string interaction_report_sha256;
    int interaction_action_count = 0;
    const bool has_interaction_member =
        provenance.hasObjectMember("interactions");
    if (options.require_interaction_report && !has_interaction_member) {
        result.error =
            "browser capture requested interactions but omitted interaction provenance";
        return result;
    }
    if (has_interaction_member) {
        if (!provenance["interactions"].isObject()) {
            result.error = "browser capture interaction provenance must be an object";
            return result;
        }
        const auto interactions = provenance["interactions"];
        interaction_report = contained_sidecar(
            envelope_path, string_member(interactions, "report"),
            result.error);
        if (!interaction_report) return result;
        if (!validate_interaction_report(
                *interaction_report, interactions,
                interaction_plan_sha256, interaction_report_sha256,
                interaction_action_count, result.error)) {
            return result;
        }
    }

    IRAssetRef backing;
    backing.asset_id = reference_id;
    backing.original_uri = "pulp-capture:///" + reference_path;
    backing.local_path = reference_png->string();
    backing.mime = "image/png";

    int matching_reference_assets = 0;
    if (envelope.hasObjectMember("assets") && envelope["assets"].isArray()) {
        const auto assets = envelope["assets"];
        for (uint32_t i = 0; i < assets.size(); ++i) {
            const auto asset = assets[static_cast<int>(i)];
            if (!asset.isObject() || string_member(asset, "id") != reference_id)
                continue;
            ++matching_reference_assets;
            if (string_member(asset, "path") != reference_path ||
                string_member(asset, "mime_type") != "image/png") {
                result.error =
                    "browser reference asset metadata does not match reference";
                return result;
            }
            backing.content_hash = string_member(asset, "sha256");
            const double width_px = number_member(asset, "width_px", -1.0);
            const double height_px = number_member(asset, "height_px", -1.0);
            const double expected_width_px = logical_width * dpr;
            const double expected_height_px = logical_height * dpr;
            if (!std::isfinite(width_px) || !std::isfinite(height_px) ||
                width_px <= 0.0 || height_px <= 0.0 ||
                std::trunc(width_px) != width_px ||
                std::trunc(height_px) != height_px ||
                width_px != expected_width_px ||
                height_px != expected_height_px ||
                width_px > std::numeric_limits<int>::max() ||
                height_px > std::numeric_limits<int>::max()) {
                result.error =
                    "browser reference asset pixel dimensions do not match "
                    "logical dimensions and DPR";
                return result;
            }
            backing.width = static_cast<int>(width_px);
            backing.height = static_cast<int>(height_px);
        }
    }
    if (matching_reference_assets != 1 || !backing.width || !backing.height ||
        backing.content_hash.size() != 64) {
        result.error = "browser reference asset metadata is incomplete";
        return result;
    }
    if (file_sha256(*reference_png) != backing.content_hash) {
        result.error = "browser reference PNG hash does not match capture envelope";
        return result;
    }
    if (!validate_png_header(
            *reference_png, *backing.width, *backing.height, result.error))
        return result;

    DesignIR ir;
    ir.source = options.source;
    ir.source_file = options.source_file;
    ir.capture_method = "chromium-cdp";
    ir.source_adapter = "browser-capture";
    ir.source_version = "pulp-browser-capture-v1";
    ir.asset_manifest.assets.push_back(std::move(backing));
    if (!load_token_report(
            *token_report, ir,
            static_cast<int>(number_member(tokens, "color_count", -1.0)),
            static_cast<int>(number_member(tokens, "dimension_count", -1.0)),
            static_cast<int>(number_member(tokens, "string_count", -1.0)),
            result.error))
        return result;

    const auto source = object_member(provenance, "source");
    if (ir.source_file.empty())
        ir.source_file = string_member(source, "entry");
    const auto settle = object_member(provenance, "settle");
    ir.settle_rounds =
        static_cast<int>(number_member(settle, "rounds", 0.0));

    // A panel smaller than the document it was captured in becomes a CLIPPED
    // frame the size of the design, holding the full capture at a negative
    // offset. The plugin then opens at the size it was designed, and the
    // pixels outside it are simply not in the window.
    const bool crop_to_surface =
        surface_width > 1.0 && surface_height > 1.0 &&
        (surface_width < logical_width - 1.0 ||
         surface_height < logical_height - 1.0);

    // The DOM snapshot is optional evidence: a capture that predates the
    // computed-style request, or names its snapshot by asset id rather than by
    // file, still lowers to a faithful capture. It just gains no appearance,
    // which is strictly what the pipeline did before.
    std::optional<CapturedStyleIndex> captured_styles;
    const std::string snapshot_asset =
        string_member(documents[static_cast<int>(0)], "snapshot_asset");
    if (!snapshot_asset.empty()) {
        std::string ignored;
        if (const auto snapshot_path =
                contained_sidecar(envelope_path, snapshot_asset, ignored)) {
            captured_styles = CapturedStyleIndex::load(*snapshot_path);
        }
    }
    // Native drawing needs the painted tree; without a snapshot there is
    // nothing to draw FROM, and silently falling back to the photograph would
    // report a native panel that is a picture.
    const bool native_lowering = options.native_panel_lowering;
    if (native_lowering && !captured_styles) {
        result.error =
            "native panel lowering requires a DOM snapshot with computed "
            "styles; this capture carries none";
        return result;
    }

    ir.root.type = "frame";
    ir.root.name = "Browser-evaluated HTML";
    if (native_lowering) {
        // No backdrop at all. The panel is its nodes.
        ir.root.style.width = static_cast<float>(
            crop_to_surface ? surface_width : logical_width);
        ir.root.style.height = static_cast<float>(
            crop_to_surface ? surface_height : logical_height);
        ir.root.style.overflow = "hidden";
        ir.root.style.position = "relative";
    } else if (crop_to_surface) {
        ir.root.style.width = static_cast<float>(surface_width);
        ir.root.style.height = static_cast<float>(surface_height);
        ir.root.style.overflow = "hidden";
        ir.root.style.position = "relative";

        pulp::view::IRNode capture;
        capture.type = "frame";
        capture.name = "Browser capture";
        capture.render_mode = NodeRenderMode::faithful_capture;
        capture.capture_asset_id = reference_id;
        capture.style.position = "absolute";
        capture.style.left = static_cast<float>(-surface_left);
        capture.style.top = static_cast<float>(-surface_top);
        capture.style.width = static_cast<float>(logical_width);
        capture.style.height = static_cast<float>(logical_height);
        capture.style.object_fit = "fill";
        capture.attributes["asset_ref"] = reference_id;
        // Every node in a lowered tree carries an anchor: it is the identity a
        // consumer edits, re-links and reconciles against. This backdrop is
        // adapter-authored rather than a document element, so the anchor is a
        // constant -- but a tree with one unanchored node is refused whole by
        // a host that enforces the contract, and the panel it refuses is the
        // one the browser just solved correctly.
        capture.stable_anchor_id = "browser:capture";
        capture.anchor_strategy = "adapter";
        ir.root.children.push_back(std::move(capture));
    } else {
        ir.root.render_mode = NodeRenderMode::faithful_capture;
        ir.root.capture_asset_id = reference_id;
        ir.root.style.width = static_cast<float>(logical_width);
        ir.root.style.height = static_cast<float>(logical_height);
        ir.root.style.object_fit = "fill";
        ir.root.style.overflow = "hidden";
    }
    ir.root.stable_anchor_id = "browser:root";
    ir.root.anchor_strategy = "adapter";
    ir.root.source_adapter = "browser-capture";
    ir.root.source_version = "pulp-browser-capture-v1";
    if (native_lowering) {
        // The reference PNG stays in the asset manifest as the A/B oracle, but
        // it is no longer what the root renders. Naming it `asset_ref` here
        // would hand the manifest pass a root that reads as an image and put
        // the photograph back on screen through the side door.
        ir.root.attributes["browser_reference_asset"] = reference_id;
    } else {
        // The typed capture_asset_id is the render contract. asset_ref keeps
        // the existing source-agnostic manifest refresh/localization pass aware
        // of the same backing without teaching that utility a browser-specific
        // field.
        ir.root.attributes["asset_ref"] = reference_id;
    }

    // The backdrop alone is a picture. These children are the live controls.
    // Their bounds are page coordinates, so when the root is cropped they must
    // move with it -- otherwise every control sits exactly one padding-width
    // down and right of the design it belongs to, which looks deliberate and
    // is not.
    const double control_dx = crop_to_surface ? -surface_left : 0.0;
    const double control_dy = crop_to_surface ? -surface_top : 0.0;
    int undeclared_paint_boxes = 0;

    // Draw the design before the controls that sit on it. Both are absolutely
    // positioned siblings, so document order IS paint order here; appending the
    // controls after the tree is what keeps a value ring above the knob face it
    // belongs to rather than under it.
    if (native_lowering) {
        const auto tree = lower_painted_tree(
            *captured_styles, control_dx, control_dy, ir.root);
        ir.root.attributes["native_painted_nodes"] =
            std::to_string(tree.painted);
        ir.root.attributes["native_nodes_lowered"] =
            std::to_string(tree.lowered);
        ir.root.attributes["native_nodes_native"] =
            std::to_string(tree.native);
        ir.root.attributes["native_nodes_image_asset"] =
            std::to_string(tree.image_asset);
        ir.root.attributes["native_nodes_element_capture_fallback"] =
            std::to_string(tree.element_capture_fallback);
        // How much of the panel those fallbacks leave BLANK, as a fraction of
        // the emitted root. The count above cannot carry that: eighteen `<svg>`
        // icons and two full-window `<canvas>` elements are both small numbers,
        // and one of them is 0.4% of the design while the other is all of it.
        // Emitted whenever there is a fallback at all, including 0.000, so a
        // consumer that reads it can tell "measured and negligible" from "the
        // producer never computed it".
        if (tree.element_capture_fallback > 0) {
            const double panel =
                static_cast<double>(ir.root.style.width.value_or(0.0f)) *
                static_cast<double>(ir.root.style.height.value_or(0.0f));
            char fraction[32] = {};
            std::snprintf(fraction, sizeof(fraction), "%.4f",
                          panel > 0.0 ? tree.unpainted_fallback_area / panel
                                      : 0.0);
            ir.root.attributes["native_nodes_unpainted_area_fraction"] =
                fraction;
        }
        ir.root.attributes["native_nodes_text"] = std::to_string(tree.text);
        ir.root.attributes["native_nodes_pooled"] =
            std::to_string(tree.pooled_into_fallback);
        // A capture that carries no paint order at all would otherwise lower to
        // pure document order and look like a plausible panel with its stacking
        // silently wrong.
        ir.root.attributes["native_nodes_missing_paint_order"] =
            std::to_string(tree.missing_paint_order);
        // The shape of the tree, not just its size. A depth of 1 means the
        // lowering flattened the design and an agent asked to "tweak the
        // pickup section" has no section to grab — the number that says so
        // belongs next to the counts, where a reviewer already looks.
        ir.root.attributes["native_tree_root_children"] =
            std::to_string(tree.root_children);
        ir.root.attributes["native_tree_depth"] =
            std::to_string(tree.max_depth);
        // Recorded only when they happened, following the same rule as the
        // controls counters: a zero here is the normal case and printing it
        // everywhere buries the one panel where a node went missing.
        const auto record_if = [&ir](const char* key, int value) {
            if (value > 0) ir.root.attributes[key] = std::to_string(value);
        };
        record_if("native_nodes_skipped_empty_box", tree.skipped_empty_box);
        record_if("native_nodes_skipped_blank_text", tree.skipped_blank_text);
        record_if("native_nodes_skipped_non_visual", tree.skipped_non_visual);
        record_if("native_nodes_hoisted", tree.hoisted_escapes);
        // Rotations solved back into a rectangle plus an angle. Reported
        // because "no fallbacks" reads the same whether a design has no
        // rotations or its rotations were recovered, and only one of those is
        // evidence the recovery ran.
        record_if("native_nodes_rotation_recovered", tree.rotation_recovered);
        record_if("native_nodes_overlapping_reorders",
                  tree.overlapping_reorders);
        // The count says a panel can paint wrong; the pairs say where. Without
        // them the only way to find an inversion is to diff two renders by eye.
        if (!tree.overlapping_reorder_pairs.empty()) {
            std::string joined;
            for (const auto& pair : tree.overlapping_reorder_pairs) {
                if (!joined.empty()) joined += ",";
                joined += pair;
            }
            ir.root.attributes["native_nodes_overlapping_reorder_pairs"] =
                joined;
        }
        // The nested tree clips by DOM parentage while CSS clips along the
        // containing-block chain, so both of these are known limitations of the
        // opt-in native path rather than transient regressions. Reported so the
        // census stops counting the affected nodes as faithfully drawn.
        record_if("native_nodes_clip_over_applied", tree.clip_over_applied);
        record_if("native_nodes_clip_lost", tree.clip_lost);
        // Inline `<svg>`, reported as three numbers rather than one: how many
        // icons became geometry, how many still arrive as a captured element,
        // and how many vector nodes the drawn ones cost. Read `svg_refused`
        // beside the per-node `capture_fallback_reason` — that string names the
        // construct that refused, which is the actual can't-draw list.
        record_if("native_svg_lowered", tree.svg_lowered);
        record_if("native_svg_refused", tree.svg_refused);
        record_if("native_svg_shapes", tree.svg_shapes);
        // A snapshot taken before the capture collected SVG paint holds the
        // geometry and no colour for it, so every icon in the design falls
        // back — and a reader sees a panel with no icons and no error, which
        // is indistinguishable from the bug this lowering exists to fix. The
        // one refusal a caller can act on gets said out loud, with the action.
        if (tree.svg_refused_stale_capture > 0) {
            // Also ON the IR, because the IR is the artifact a render harness
            // dumps and greps. A warning only the CLI prints is invisible to
            // every caller that lowers in-process, which is how this went
            // unnoticed for a whole debugging round.
            ir.root.attributes["native_svg_stale_capture"] =
                std::to_string(tree.svg_refused_stale_capture);
            result.warnings.push_back(
                "capture predates the SVG paint protocol: " +
                std::to_string(tree.svg_refused_stale_capture) + " of " +
                std::to_string(tree.svg_refused + tree.svg_lowered) +
                " inline <svg> element(s) kept their captured pixels because "
                "this snapshot carries no resolved fill/stroke. Re-run the "
                "browser capture to draw them.");
        }

        // Same failure, different field. A basis with no resolved face is one
        // the renderer refuses, so the run re-derives its own line breaking —
        // and a run that resumes mid-line after an inline `<span>` loses the
        // offset that placed it and prints over its own sibling. That reads as
        // a text-layout bug, and it is a capture missing one column.
        if (tree.text_line_boxes_without_face > 0) {
            ir.root.attributes["native_text_stale_capture"] =
                std::to_string(tree.text_line_boxes_without_face);
            result.warnings.push_back(
                "capture predates the resolved-font-face protocol: " +
                std::to_string(tree.text_line_boxes_without_face) +
                " text run(s) carry captured line boxes with no face, so their "
                "line breaking is re-derived rather than reproduced. Re-run the "
                "browser capture to use the browser's own line breaks.");
        }
        record_if("native_nodes_type_scaled", tree.type_scaled);
        record_if("native_nodes_type_scale_refused", tree.type_scale_refused);
    }

    int styled_controls = 0;
    const int lowered = lower_semantic_controls(
        *semantic_report, ir, control_dx, control_dy, dpr, ir.root,
        captured_styles ? &*captured_styles : nullptr, native_lowering,
        styled_controls, undeclared_paint_boxes, result.error);
    if (lowered < 0) return result;
    ir.root.attributes["controls_lowered"] = std::to_string(lowered);
    // Recorded so a capture that silently lost its solved appearance is
    // visible as a number rather than as a flat-looking panel nobody can
    // attribute.
    ir.root.attributes["controls_with_captured_style"] =
        std::to_string(styled_controls);
    // Surfaced rather than swallowed: an undeclared paint box means the widget
    // was placed on the component box, which for a captioned control paints its
    // value geometry low. Correct for a meter, wrong for a knob, and nothing
    // downstream can tell the difference.
    if (undeclared_paint_boxes > 0)
        ir.root.attributes["controls_without_paint_box"] =
            std::to_string(undeclared_paint_boxes);
    // These are evidence identifiers, not filesystem dependencies. Keep them
    // portable when the DesignIR and its localized asset directory move.
    ir.root.attributes["browser_capture_envelope"] =
        "pulp-capture:///capture.json";
    ir.root.attributes["browser_semantic_report"] =
        "pulp-capture:///" + semantic_path;
    ir.root.attributes["browser_token_report"] =
        "pulp-capture:///" + token_path;
    if (interaction_report) {
        ir.root.attributes["browser_interaction_report"] =
            "pulp-capture:///" +
            string_member(provenance["interactions"], "report");
        ir.root.attributes["browser_interaction_report_sha256"] =
            interaction_report_sha256;
        ir.root.attributes["browser_interaction_plan_sha256"] =
            interaction_plan_sha256;
        ir.root.attributes["browser_interaction_action_count"] =
            std::to_string(interaction_action_count);
    }
    ir.root.attributes["browser_semantic_candidates"] =
        std::to_string(static_cast<int>(
            number_member(semantics, "candidate_count", 0.0)));
    ir.root.attributes["browser_semantic_resolved"] =
        std::to_string(static_cast<int>(
            number_member(semantics, "resolved_count", 0.0)));
    ir.root.attributes["browser_semantic_unresolved"] =
        std::to_string(static_cast<int>(
            number_member(semantics, "unresolved_count", 0.0)));
    ir.root.attributes["browser_device_scale_factor"] =
        std::to_string(dpr);
    // Carry the registration rect onto the root so a consumer holding only the
    // IR can put its render and the reference over the same pixels. Recorded in
    // CSS px next to the DPR that scales them, so the envelope keeps one device
    // scale and there is no second copy to drift from it.
    //
    // The primary surface is the same rect measured a different way, and it is
    // what this root's own geometry was derived from just above -- so a capture
    // that predates reference.authored_frame still registers, instead of the
    // consumer refusing every panel captured before the field existed.
    const auto record_authored_frame =
        [&ir](double x, double y, double width, double height) {
            ir.root.attributes["browser_authored_frame_x"] = std::to_string(x);
            ir.root.attributes["browser_authored_frame_y"] = std::to_string(y);
            ir.root.attributes["browser_authored_frame_width"] =
                std::to_string(width);
            ir.root.attributes["browser_authored_frame_height"] =
                std::to_string(height);
        };
    if (has_authored_frame) {
        record_authored_frame(number_member(authored_frame, "x", 0.0),
                              number_member(authored_frame, "y", 0.0),
                              authored_frame_width, authored_frame_height);
    } else if (crop_to_surface) {
        record_authored_frame(surface_left, surface_top, surface_width,
                              surface_height);
    }

    result.reference_png = *reference_png;
    result.semantic_report = *semantic_report;
    result.interaction_report = std::move(interaction_report);
    result.design_ir = std::move(ir);
    return result;
}

}  // namespace pulp::import_design
