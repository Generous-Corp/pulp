#include "delay_time_model.hpp"

#include "delay_params.hpp"

#include <pulp/state/store.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pulp::examples::delay {

namespace {

constexpr std::array<double, 11> kDivisionBeats = {
    0.125, 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.5, 0.75, 2.0 / 3.0, 1.0, 1.5, 2.0, 4.0,
};

constexpr float kMinTimeMs = 1.0f;
constexpr float kMaxTimeMs = 2000.0f;
constexpr float kMaxAddressableTimeMs = 3000.0f;

double valid_tempo(double bpm) noexcept {
    return std::isfinite(bpm) && bpm > 0.0 ? bpm : DelayTimeModel::kFallbackTempoBpm;
}

float clamp_left(float milliseconds) noexcept {
    return std::clamp(milliseconds, kMinTimeMs, kMaxTimeMs);
}

float clamp_right(float milliseconds) noexcept {
    return std::clamp(milliseconds, kMinTimeMs, kMaxAddressableTimeMs);
}

} // namespace

double DelayTimeModel::division_beats(int index) noexcept {
    const auto bounded = std::clamp(index, 0, static_cast<int>(kDivisionBeats.size()) - 1);
    return kDivisionBeats[static_cast<std::size_t>(bounded)];
}

float DelayTimeModel::synced_time_ms(int index, double tempo_bpm) noexcept {
    return clamp_left(static_cast<float>(division_beats(index) * 60000.0 / valid_tempo(tempo_bpm)));
}

RightTimingBranch DelayTimeModel::right_timing_branch(
    const DelayTimeInputs& inputs) noexcept {
    if (inputs.routing == Routing::ping_pong)
        return RightTimingBranch::ping_pong;
    if (inputs.link && inputs.offset_mode == OffsetMode::ratio)
        return RightTimingBranch::linked_ratio;
    if (inputs.link)
        return RightTimingBranch::linked_offset_ms;
    return inputs.sync ? RightTimingBranch::synced_independent
                       : RightTimingBranch::free_independent;
}

EffectiveDelayTimes DelayTimeModel::derive(const DelayTimeInputs& inputs) noexcept {
    EffectiveDelayTimes result;
    result.left_ms = inputs.sync ? synced_time_ms(inputs.division, inputs.tempo_bpm)
                                 : clamp_left(inputs.time_ms);

    switch (right_timing_branch(inputs)) {
    case RightTimingBranch::ping_pong:
        result.right_ms = result.left_ms;
        result.right_uses_ratio = true;
        result.right_ratio = 1.0f;
        return result;
    case RightTimingBranch::linked_ratio:
        result.right_ratio = std::clamp(inputs.time_offset, 0.5f, 1.5f);
        result.right_ms = clamp_right(result.left_ms * result.right_ratio);
        result.right_uses_ratio = true;
        return result;
    case RightTimingBranch::linked_offset_ms:
        result.right_ms = clamp_right(result.left_ms + inputs.offset_ms);
        break;
    case RightTimingBranch::synced_independent:
        result.right_ms = synced_time_ms(inputs.division_right, inputs.tempo_bpm);
        break;
    case RightTimingBranch::free_independent:
        result.right_ms = clamp_right(inputs.time_right_ms);
        break;
    }
    result.right_uses_ratio = false;
    result.right_ratio = result.right_ms / result.left_ms;
    return result;
}

DelayTimeInputs delay_time_inputs_from_store(const state::StateStore& store,
                                              double tempo_bpm) noexcept {
    return {
        .time_ms = store.get_value(kTime),
        .sync = store.get_value(kSync) >= 0.5f,
        .division = division_index_from_param(store.get_value(kDivision)),
        .link = store.get_value(kLink) >= 0.5f,
        .offset_mode = offset_mode_from_param(store.get_value(kOffsetMode)),
        .time_offset = store.get_value(kTimeOffset),
        .offset_ms = store.get_value(kOffsetMs),
        .time_right_ms = store.get_value(kTimeRight),
        .division_right =
            division_index_from_param(store.get_value(kDivisionRight)),
        .routing = routing_from_param(store.get_value(kRouting)),
        .tempo_bpm = tempo_bpm,
    };
}

} // namespace pulp::examples::delay
