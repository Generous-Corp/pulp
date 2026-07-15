#pragma once
/// @file stretch_preset.hpp
/// A shareable fine-tune layer ON TOP of OfflineStretch — it does NOT fork the
/// core engine. A `StretchPreset` is a flat, human-editable spec of the tunable
/// knobs (character mode, window/overlap, transient sensitivity, STN routing,
/// relocation). It serializes to a tiny `key = value` text format so a user (or
/// their agent) can save a render they like, override the out-of-box defaults per
/// plugin, hand-edit it, and SHARE it ("I think this sounds better"). Apply it to
/// an OfflineStretchOptions just before process().
///
/// Format (lines, `#` comments, order-independent, unknown keys ignored):
///   name = My Tape Preset
///   character = varispeed        # clean | varispeed | phase_vocoder | granular
///   fft_size = 0                 # 0 = adaptive
///   analysis_hop = 0
///   transient_sensitivity = 0    # 0 = engine default
///   route_noise_stn = false
///   relocate_transients = false

#include <pulp/signal/offline_stretch.hpp>

#include <sstream>
#include <string>

namespace pulp::signal {

/// The tunable subset of OfflineStretchOptions that defines a "character preset".
/// Defaults mirror the engine defaults, so a freshly-constructed preset is the
/// out-of-box behavior.
struct StretchPreset {
    std::string name = "untitled";
    StretchCharacter character = StretchCharacter::clean;
    int fft_size = 0;                  ///< 0 = adaptive (recommend_window)
    int analysis_hop = 0;
    float transient_sensitivity = 0.0f;///< 0 = engine default
    bool route_noise_stn = false;
    bool relocate_transients = false;
};

inline const char* to_string(StretchCharacter c) {
    switch (c) {
        case StretchCharacter::varispeed:     return "varispeed";
        case StretchCharacter::phase_vocoder: return "phase_vocoder";
        case StretchCharacter::granular:      return "granular";
        case StretchCharacter::clean:         default: return "clean";
    }
}

inline bool character_from_string(const std::string& s, StretchCharacter& out) {
    if (s == "clean")         { out = StretchCharacter::clean; return true; }
    if (s == "varispeed")     { out = StretchCharacter::varispeed; return true; }
    if (s == "phase_vocoder") { out = StretchCharacter::phase_vocoder; return true; }
    if (s == "granular")      { out = StretchCharacter::granular; return true; }
    return false;
}

/// Capture the preset-relevant fields FROM a configured OfflineStretchOptions.
inline StretchPreset capture_preset(const OfflineStretchOptions& o,
                                    const std::string& name = "untitled") {
    StretchPreset p;
    p.name = name;
    p.character = o.character;
    p.fft_size = o.fft_size;
    p.analysis_hop = o.analysis_hop;
    p.transient_sensitivity = o.transient_sensitivity;
    p.route_noise_stn = o.route_noise_stn;
    p.relocate_transients = o.relocate_transients;
    return p;
}

/// Apply a preset to options (only the preset-managed fields; ratio/pitch are the
/// caller's, not the preset's).
inline void apply_preset(OfflineStretchOptions& o, const StretchPreset& p) {
    o.character = p.character;
    o.fft_size = p.fft_size;
    o.analysis_hop = p.analysis_hop;
    o.transient_sensitivity = p.transient_sensitivity;
    o.route_noise_stn = p.route_noise_stn;
    o.relocate_transients = p.relocate_transients;
}

inline std::string preset_to_text(const StretchPreset& p) {
    std::ostringstream s;
    s << "# Pulp stretch preset\n"
      << "name = " << p.name << "\n"
      << "character = " << to_string(p.character) << "\n"
      << "fft_size = " << p.fft_size << "\n"
      << "analysis_hop = " << p.analysis_hop << "\n"
      << "transient_sensitivity = " << p.transient_sensitivity << "\n"
      << "route_noise_stn = " << (p.route_noise_stn ? "true" : "false") << "\n"
      << "relocate_transients = " << (p.relocate_transients ? "true" : "false") << "\n";
    return s.str();
}

/// Parse a preset. Tolerant: blank lines and `#` comments skipped, unknown keys
/// ignored, whitespace trimmed. Returns false (with *err set) only on a malformed
/// value for a recognized key (e.g. a bad character name).
inline bool preset_from_text(const std::string& text, StretchPreset& out,
                             std::string* err = nullptr) {
    auto trim = [](std::string v) {
        const auto a = v.find_first_not_of(" \t\r\n");
        const auto b = v.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
    };
    auto as_bool = [](const std::string& v) { return v == "true" || v == "1" || v == "yes"; };
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(t.substr(0, eq));
        const std::string val = trim(t.substr(eq + 1));
        if (key == "name") out.name = val;
        else if (key == "character") {
            if (!character_from_string(val, out.character)) {
                if (err) *err = "unknown character: " + val;
                return false;
            }
        }
        else if (key == "fft_size") out.fft_size = std::atoi(val.c_str());
        else if (key == "analysis_hop") out.analysis_hop = std::atoi(val.c_str());
        else if (key == "transient_sensitivity") out.transient_sensitivity = static_cast<float>(std::atof(val.c_str()));
        else if (key == "route_noise_stn") out.route_noise_stn = as_bool(val);
        else if (key == "relocate_transients") out.relocate_transients = as_bool(val);
        // unknown keys ignored (forward-compatible)
    }
    return true;
}

} // namespace pulp::signal
