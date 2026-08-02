#pragma once

#include <pulp/timeline/model.hpp>

#include <optional>
#include <string_view>

namespace pulp::timeline::detail {

// The wire spelling of the document enums that are neither chord nor scale
// vocabulary. Names rather than ordinals, on the same terms as
// chord_scale_names.hpp: inserting a value later cannot renumber a saved
// document. Both directions live here so the encoder and decoder cannot drift.

constexpr std::string_view section_role_name(SectionRole role) noexcept {
    switch (role) {
    case SectionRole::Unspecified:
        return "unspecified";
    case SectionRole::Intro:
        return "intro";
    case SectionRole::Verse:
        return "verse";
    case SectionRole::PreChorus:
        return "pre_chorus";
    case SectionRole::Chorus:
        return "chorus";
    case SectionRole::Bridge:
        return "bridge";
    case SectionRole::Breakdown:
        return "breakdown";
    case SectionRole::Drop:
        return "drop";
    case SectionRole::Solo:
        return "solo";
    case SectionRole::Interlude:
        return "interlude";
    case SectionRole::Outro:
        return "outro";
    }
    return {};
}

constexpr std::optional<SectionRole> section_role_from_name(std::string_view name) noexcept {
    if (name == "unspecified")
        return SectionRole::Unspecified;
    if (name == "intro")
        return SectionRole::Intro;
    if (name == "verse")
        return SectionRole::Verse;
    if (name == "pre_chorus")
        return SectionRole::PreChorus;
    if (name == "chorus")
        return SectionRole::Chorus;
    if (name == "bridge")
        return SectionRole::Bridge;
    if (name == "breakdown")
        return SectionRole::Breakdown;
    if (name == "drop")
        return SectionRole::Drop;
    if (name == "solo")
        return SectionRole::Solo;
    if (name == "interlude")
        return SectionRole::Interlude;
    if (name == "outro")
        return SectionRole::Outro;
    return std::nullopt;
}

constexpr std::string_view tuning_system_name(TuningSystem system) noexcept {
    switch (system) {
    case TuningSystem::EqualTemperament:
        return "equal_temperament";
    case TuningSystem::MtsEsp:
        return "mts_esp";
    case TuningSystem::Scala:
        return "scala";
    }
    return {};
}

constexpr std::optional<TuningSystem> tuning_system_from_name(std::string_view name) noexcept {
    if (name == "equal_temperament")
        return TuningSystem::EqualTemperament;
    if (name == "mts_esp")
        return TuningSystem::MtsEsp;
    if (name == "scala")
        return TuningSystem::Scala;
    return std::nullopt;
}

} // namespace pulp::timeline::detail
