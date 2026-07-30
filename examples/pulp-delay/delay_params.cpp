#include "delay_params.hpp"

#include <pulp/state/store.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace pulp::examples::delay {

namespace {

state::ParamRange percent(float default_value) {
    return state::ParamRange::linear(0.0f, 100.0f, default_value, 0.1f);
}

state::ParamInfo choice(state::ParamID id, std::string name, std::vector<std::string> labels,
                        float default_value) {
    return {
        .id = id,
        .name = std::move(name),
        .unit = "",
        .range = state::ParamRange::linear(0.0f, static_cast<float>(labels.size() - 1),
                                           default_value, 1.0f),
        .kind = state::ParamKind::Enum,
        .value_labels = std::move(labels),
    };
}

state::ParamInfo toggle(state::ParamID id, std::string name, bool default_value) {
    return {
        .id = id,
        .name = std::move(name),
        .unit = "",
        .range = state::ParamRange::linear(0.0f, 1.0f, default_value ? 1.0f : 0.0f, 1.0f),
        .kind = state::ParamKind::Toggle,
        .value_labels = {"Off", "On"},
    };
}

} // namespace

void define_delay_parameters(state::StateStore& store) {
    store.add_parameter({
        .id = kTime,
        .name = "Time",
        .unit = "ms",
        .range = state::ParamRange::with_center(1.0f, 2000.0f, 250.0f, 380.0f, 1.0f),
    });
    store.add_parameter({
        .id = kFeedback,
        .name = "Feedback",
        .unit = "%",
        .range = state::ParamRange::linear(0.0f, 110.0f, 62.0f, 0.1f),
    });
    store.add_parameter(choice(kCharacter, "Character", {"Clean", "Vintage", "Tape", "BBD"},
                               static_cast<float>(Character::tape)));
    store.add_parameter({
        .id = kMix,
        .name = "Mix",
        .unit = "%",
        .range = percent(45.0f),
    });
    store.add_parameter({
        .id = kCharacterAmount,
        .name = "Character Amount",
        .unit = "%",
        .range = percent(58.0f),
    });
    store.add_parameter({
        .id = kDiffusion,
        .name = "Diffusion",
        .unit = "%",
        .range = percent(22.0f),
    });
    store.add_parameter(toggle(kSync, "Sync", false));
    store.add_parameter(
        choice(kDivision, "Division",
               std::vector<std::string>(kDivisionLabels.begin(), kDivisionLabels.end()), 4.0f));
    store.add_parameter(toggle(kLink, "Link", true));
    store.add_parameter(choice(kOffsetMode, "Offset Mode", {"Ratio", "Milliseconds"},
                               static_cast<float>(OffsetMode::ratio)));
    store.add_parameter({
        .id = kTimeOffset,
        .name = "Time Offset",
        .unit = "x",
        .range = state::ParamRange::linear(0.5f, 1.5f, 1.12f, 0.001f),
    });
    store.add_parameter({
        .id = kOffsetMs,
        .name = "Offset ms",
        .unit = "ms",
        .range = state::ParamRange::linear(-50.0f, 50.0f, 14.0f, 1.0f),
    });
    store.add_parameter({
        .id = kTimeRight,
        .name = "Time R",
        .unit = "ms",
        .range = state::ParamRange::with_center(1.0f, 2000.0f, 250.0f, 620.0f, 1.0f),
    });
    store.add_parameter(
        choice(kDivisionRight, "Division R",
               std::vector<std::string>(kDivisionLabels.begin(), kDivisionLabels.end()), 6.0f));
    store.add_parameter(choice(kRouting, "Routing", {"Mono", "Stereo", "Ping-Pong"},
                               static_cast<float>(Routing::stereo)));
    store.add_parameter({
        .id = kCrossfeed,
        .name = "Crossfeed",
        .unit = "%",
        .range = percent(48.0f),
    });
    store.add_parameter({
        .id = kDuck,
        .name = "Duck",
        .unit = "%",
        .range = percent(34.0f),
    });
    store.add_parameter({
        .id = kLowCut,
        .name = "Low Cut",
        .unit = "Hz",
        .range = state::ParamRange::with_center(20.0f, 2000.0f, 200.0f, 180.0f, 1.0f),
    });
    store.add_parameter({
        .id = kHighCut,
        .name = "High Cut",
        .unit = "Hz",
        .range = state::ParamRange::with_center(200.0f, 20000.0f, 2000.0f, 4800.0f, 1.0f),
    });
    store.add_parameter({
        .id = kLowCutResonance,
        .name = "Low Cut Res",
        .unit = "Q",
        .range = state::ParamRange::linear(0.5f, 2.0f, 1.2f, 0.01f),
    });
    store.add_parameter({
        .id = kHighCutResonance,
        .name = "High Cut Res",
        .unit = "Q",
        .range = state::ParamRange::linear(0.5f, 2.0f, 0.8f, 0.01f),
    });
    store.add_parameter({
        .id = kModRate,
        .name = "Mod Rate",
        .unit = "Hz",
        .range = state::ParamRange::with_center(0.05f, 10.0f, 0.7071f, 0.42f, 0.01f),
    });
    store.add_parameter({
        .id = kModDepth,
        .name = "Mod Depth",
        .unit = "%",
        .range = percent(28.0f),
    });
    store.add_parameter(toggle(kFreeze, "Freeze", false));
    store.add_parameter(toggle(kReverse, "Reverse", false));
}

Character character_from_param(float value) noexcept {
    const int index = std::clamp(static_cast<int>(std::lround(value)), 0, 3);
    return static_cast<Character>(index);
}

OffsetMode offset_mode_from_param(float value) noexcept {
    return value >= 0.5f ? OffsetMode::milliseconds : OffsetMode::ratio;
}

Routing routing_from_param(float value) noexcept {
    const int index = std::clamp(static_cast<int>(std::lround(value)), 0, 2);
    return static_cast<Routing>(index);
}

int division_index_from_param(float value) noexcept {
    return std::clamp(static_cast<int>(std::lround(value)), 0,
                      static_cast<int>(kDivisionLabels.size()) - 1);
}

} // namespace pulp::examples::delay
