#pragma once

/// @file module_descriptor.hpp
/// The layout manifest: one declarative description of a modular module, from
/// which the panel SVG, the `plugin.json` entry, and the widget placement are
/// all derived.
///
/// This header deliberately has NO dependency on the Rack SDK. It is pure data
/// plus pure functions, so the manifest can be authored, validated and unit
/// tested in a build with `PULP_HAS_RACK=OFF` — which is every build that does
/// not have a developer-supplied SDK. Only the adapter that turns a manifest
/// into a live `rack::engine::Module` includes `<rack.hpp>`.
///
/// Why a manifest at all: the panel art and the C++ widget placement must agree
/// on coordinates to a fraction of a millimetre, and a label whose baseline
/// lands inside a widget's footprint is invisible in Rack — the widget is drawn
/// on top of the panel and does not exist in the SVG. Deriving both sides from
/// one description is what makes that class of mismatch unrepresentable.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace pulp::format::rack {

/// Eurorack geometry. 1 HP is 5.08 mm; a 3U panel is 380 px at Rack's 75 dpi,
/// which is 128.6933 mm — NOT the 128.5 mm quoted in the VCV manual. Authoring
/// at 128.5 mm renders a panel one pixel short.
inline constexpr double kHpMm = 5.08;
inline constexpr double kPanelHeightMm = 380.0 / 75.0 * 25.4;  // 128.69333…
inline constexpr double kRackDpi = 75.0;

inline constexpr double hp_to_mm(int hp) { return hp * kHpMm; }
inline constexpr double mm_to_px(double mm) { return mm / 25.4 * kRackDpi; }

/// Published modular voltage standards. Centralized so no module can drift.
namespace volts {
inline constexpr float kAudioPeak = 5.0f;     ///< audio is ±5 V
inline constexpr float kCvUnipolar = 10.0f;   ///< unipolar CV is 0..10 V
inline constexpr float kCvBipolar = 5.0f;     ///< bipolar CV is ±5 V
inline constexpr float kGateHigh = 10.0f;     ///< gates and triggers are 10 V
inline constexpr float kSchmittLow = 0.1f;    ///< trigger detection thresholds
inline constexpr float kSchmittHigh = 1.0f;
/// 1 V/oct: f = f0 · 2^V. Audio oscillators reference C4; LFOs and clocks
/// reference 2 Hz (120 BPM).
inline constexpr float kPitchRefHz = 261.6256f;
inline constexpr float kLfoRefHz = 2.0f;
inline float voct_to_hz(float volts, float ref_hz = kPitchRefHz) {
    return ref_hz * std::pow(2.0f, volts);
}
}  // namespace volts

/// Which physical control renders a parameter. Determines both the panel
/// footprint and the Rack widget class, so the two cannot disagree.
enum class ControlKind {
    KnobLarge,   ///< 54 px / 18.29 mm — the hero control
    Knob,        ///< 36 px / 12.19 mm — standard
    KnobSmall,   ///< 25.5 px / 8.64 mm
    Trimpot,     ///< 17 px / 5.76 mm — attenuverters and secondary amounts
    Toggle,      ///< 10 × 20.6 px switch
    Button,      ///< momentary
};

/// What a jack carries. Drives the label, the tooltip, and the panel's
/// input/output treatment — outputs sit on a filled shape, inputs on bare
/// plate, so the distinction survives greyscale and a dark room.
enum class PortRole {
    Audio,    ///< ±5 V
    Cv,       ///< 0..10 V or ±5 V
    Pitch,    ///< 1 V/oct
    Gate,     ///< 10 V while active
    Trigger,  ///< 10 V, ~1 ms
    Clock,    ///< trigger used as a time base
};

/// Drawn footprint radius in mm for each control kind — the radius of what
/// Rack actually paints, not of the authoring placeholder. Label clearance is
/// computed from this, which is the check an SVG-only validator cannot make.
inline constexpr double control_radius_mm(ControlKind k) {
    switch (k) {
        case ControlKind::KnobLarge: return 9.15;   // 54 px / 2
        case ControlKind::Knob:      return 6.10;   // 36 px / 2
        case ControlKind::KnobSmall: return 4.32;   // 25.5 px / 2
        case ControlKind::Trimpot:   return 2.88;   // 17 px / 2
        case ControlKind::Toggle:    return 3.49;   // 20.6 px / 2 (tall axis)
        case ControlKind::Button:    return 3.05;
    }
    return 6.10;
}

/// Jack footprint (PJ301M is 24.3 px square).
inline constexpr double kJackRadiusMm = 4.11;
/// Medium LED (3 mm).
inline constexpr double kLightRadiusMm = 1.39;

/// The Rack widget class this control maps to. Kept as a string because it is
/// emitted into generated C++ and named in the panel manifest; the mapping is
/// data, not a switch buried in the code generator.
inline const char* widget_class(ControlKind k) {
    switch (k) {
        case ControlKind::KnobLarge: return "RoundBigBlackKnob";
        case ControlKind::Knob:      return "RoundBlackKnob";
        case ControlKind::KnobSmall: return "RoundSmallBlackKnob";
        case ControlKind::Trimpot:   return "Trimpot";
        case ControlKind::Toggle:    return "CKSS";
        case ControlKind::Button:    return "VCVButton";
    }
    return "RoundBlackKnob";
}

struct ParamSpec {
    int id = 0;
    std::string ident;             ///< enum identifier, e.g. "FREQ_PARAM"
    std::string name;              ///< tooltip name, e.g. "Frequency"
    std::string label;             ///< panel lettering, e.g. "FREQ" (may be empty)
    std::string unit;              ///< tooltip unit, e.g. " Hz"
    float min_value = 0.0f;
    float max_value = 1.0f;
    float default_value = 0.0f;
    ControlKind kind = ControlKind::Knob;
    double x_mm = 0.0;
    double y_mm = 0.0;
    /// Exponential display base for frequency-style params (0 = linear).
    float display_base = 0.0f;
    float display_multiplier = 1.0f;
    bool snap = false;             ///< integer-quantized (step counts, divisions)
};

struct PortSpec {
    int id = 0;
    std::string ident;             ///< e.g. "VOCT_INPUT"
    std::string name;              ///< tooltip, e.g. "1V/oct pitch"
    std::string label;             ///< panel lettering, e.g. "V/OCT"
    PortRole role = PortRole::Audio;
    double x_mm = 0.0;
    double y_mm = 0.0;
};

struct LightSpec {
    int id = 0;
    std::string ident;
    std::string name;
    std::string label;
    double x_mm = 0.0;
    double y_mm = 0.0;
};

/// One module: everything needed to emit its panel, its manifest entry, and
/// its widget placement.
struct ModuleDescriptor {
    std::string slug;              ///< PERMANENT once published — see the identity policy
    std::string name;              ///< human-readable, shown in the browser
    std::string description;
    std::vector<std::string> tags; ///< must come from Rack's fixed vocabulary
    int hp = 8;
    std::vector<ParamSpec> params;
    std::vector<PortSpec> inputs;
    std::vector<PortSpec> outputs;
    std::vector<LightSpec> lights;

    double width_mm() const { return hp_to_mm(hp); }
};

/// A whole plugin: brand plus its modules.
struct PluginManifest {
    std::string slug;              ///< PERMANENT once published
    std::string name;
    std::string brand;             ///< prefix shown in the Module Browser
    std::string version = "2.0.0"; ///< major must match Rack's major
    std::string license = "MIT";
    std::string author;
    std::string author_email;
    std::string author_url;
    std::string plugin_url;
    std::string manual_url;
    std::string source_url;
    std::vector<ModuleDescriptor> modules;
};

}  // namespace pulp::format::rack
