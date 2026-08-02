#pragma once

#include <pulp/timeline/model.hpp>

#include <optional>
#include <string_view>

namespace pulp::timeline::detail {

// The wire spelling of the chord/scale enums. Names rather than ordinals, so
// inserting a quality or mode later cannot renumber a saved document, and a
// diff of two saves reads as music.
//
// Both directions live here so the encoder and the decoder cannot drift into
// disagreeing about a name; the round-trip test asserts every value survives.

constexpr std::string_view chord_quality_name(ChordQuality quality) noexcept {
    switch (quality) {
    case ChordQuality::Major:
        return "major";
    case ChordQuality::Minor:
        return "minor";
    case ChordQuality::Diminished:
        return "diminished";
    case ChordQuality::Augmented:
        return "augmented";
    case ChordQuality::Dominant7:
        return "dominant7";
    case ChordQuality::Major7:
        return "major7";
    case ChordQuality::Minor7:
        return "minor7";
    case ChordQuality::HalfDiminished7:
        return "half_diminished7";
    case ChordQuality::Suspended2:
        return "suspended2";
    case ChordQuality::Suspended4:
        return "suspended4";
    }
    return {};
}

constexpr std::optional<ChordQuality> chord_quality_from_name(std::string_view name) noexcept {
    if (name == "major")
        return ChordQuality::Major;
    if (name == "minor")
        return ChordQuality::Minor;
    if (name == "diminished")
        return ChordQuality::Diminished;
    if (name == "augmented")
        return ChordQuality::Augmented;
    if (name == "dominant7")
        return ChordQuality::Dominant7;
    if (name == "major7")
        return ChordQuality::Major7;
    if (name == "minor7")
        return ChordQuality::Minor7;
    if (name == "half_diminished7")
        return ChordQuality::HalfDiminished7;
    if (name == "suspended2")
        return ChordQuality::Suspended2;
    if (name == "suspended4")
        return ChordQuality::Suspended4;
    return std::nullopt;
}

constexpr std::string_view scale_mode_name(ScaleMode mode) noexcept {
    switch (mode) {
    case ScaleMode::Major:
        return "major";
    case ScaleMode::NaturalMinor:
        return "natural_minor";
    case ScaleMode::HarmonicMinor:
        return "harmonic_minor";
    case ScaleMode::MelodicMinor:
        return "melodic_minor";
    case ScaleMode::Dorian:
        return "dorian";
    case ScaleMode::Phrygian:
        return "phrygian";
    case ScaleMode::Lydian:
        return "lydian";
    case ScaleMode::Mixolydian:
        return "mixolydian";
    case ScaleMode::Locrian:
        return "locrian";
    case ScaleMode::Chromatic:
        return "chromatic";
    }
    return {};
}

constexpr std::optional<ScaleMode> scale_mode_from_name(std::string_view name) noexcept {
    if (name == "major")
        return ScaleMode::Major;
    if (name == "natural_minor")
        return ScaleMode::NaturalMinor;
    if (name == "harmonic_minor")
        return ScaleMode::HarmonicMinor;
    if (name == "melodic_minor")
        return ScaleMode::MelodicMinor;
    if (name == "dorian")
        return ScaleMode::Dorian;
    if (name == "phrygian")
        return ScaleMode::Phrygian;
    if (name == "lydian")
        return ScaleMode::Lydian;
    if (name == "mixolydian")
        return ScaleMode::Mixolydian;
    if (name == "locrian")
        return ScaleMode::Locrian;
    if (name == "chromatic")
        return ScaleMode::Chromatic;
    return std::nullopt;
}

constexpr std::string_view chord_voicing_name(ChordVoicing voicing) noexcept {
    switch (voicing) {
    case ChordVoicing::Close:
        return "close";
    case ChordVoicing::Open:
        return "open";
    case ChordVoicing::Drop2:
        return "drop2";
    case ChordVoicing::Drop3:
        return "drop3";
    case ChordVoicing::Rootless:
        return "rootless";
    case ChordVoicing::Shell:
        return "shell";
    }
    return {};
}

constexpr std::optional<ChordVoicing> chord_voicing_from_name(std::string_view name) noexcept {
    if (name == "close")
        return ChordVoicing::Close;
    if (name == "open")
        return ChordVoicing::Open;
    if (name == "drop2")
        return ChordVoicing::Drop2;
    if (name == "drop3")
        return ChordVoicing::Drop3;
    if (name == "rootless")
        return ChordVoicing::Rootless;
    if (name == "shell")
        return ChordVoicing::Shell;
    return std::nullopt;
}

} // namespace pulp::timeline::detail
