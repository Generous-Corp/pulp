#pragma once

#include <pulp/music/chord.hpp>
#include <pulp/music/pitch.hpp>
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

constexpr std::optional<music::ChordQuality> music_chord_quality(ChordQuality quality) noexcept {
    const auto index = static_cast<std::size_t>(quality);
    if (index >= music::kPulpTimelineChordQualities.size())
        return std::nullopt;
    return music::kPulpTimelineChordQualities[index].quality;
}

constexpr std::string_view chord_quality_name(ChordQuality quality) noexcept {
    const auto index = static_cast<std::size_t>(quality);
    if (index >= music::kPulpTimelineChordQualities.size())
        return {};
    return music::kPulpTimelineChordQualities[index].stored_name;
}

constexpr std::optional<ChordQuality> chord_quality_from_name(std::string_view name) noexcept {
    for (std::size_t index = 0; index < music::kPulpTimelineChordQualities.size(); ++index)
        if (music::kPulpTimelineChordQualities[index].stored_name == name)
            return static_cast<ChordQuality>(index);
    return std::nullopt;
}

constexpr std::optional<music::NamedScale> music_scale_mode(ScaleMode mode) noexcept {
    const auto index = static_cast<std::size_t>(mode);
    if (index >= music::kPulpTimelineScales.size())
        return std::nullopt;
    return music::kPulpTimelineScales[index].scale;
}

constexpr std::string_view scale_mode_name(ScaleMode mode) noexcept {
    const auto index = static_cast<std::size_t>(mode);
    if (index >= music::kPulpTimelineScales.size())
        return {};
    return music::kPulpTimelineScales[index].stored_name;
}

constexpr std::optional<ScaleMode> scale_mode_from_name(std::string_view name) noexcept {
    for (std::size_t index = 0; index < music::kPulpTimelineScales.size(); ++index)
        if (music::kPulpTimelineScales[index].stored_name == name)
            return static_cast<ScaleMode>(index);
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
