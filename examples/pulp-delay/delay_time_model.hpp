#pragma once

#include "delay_types.hpp"

namespace pulp::examples::delay {

struct DelayTimeInputs {
    float time_ms = 380.0f;
    bool sync = false;
    int division = 4;
    bool link = true;
    OffsetMode offset_mode = OffsetMode::ratio;
    float time_offset = 1.12f;
    float offset_ms = 14.0f;
    float time_right_ms = 620.0f;
    int division_right = 6;
    Routing routing = Routing::stereo;
    double tempo_bpm = 120.0;
};

struct EffectiveDelayTimes {
    float left_ms = 380.0f;
    float right_ms = 425.6f;
    bool right_uses_ratio = true;
    float right_ratio = 1.12f;
};

class DelayTimeModel {
  public:
    static constexpr double kFallbackTempoBpm = 120.0;

    static double division_beats(int index) noexcept;
    static float synced_time_ms(int index, double tempo_bpm) noexcept;
    static EffectiveDelayTimes derive(const DelayTimeInputs& inputs) noexcept;
};

} // namespace pulp::examples::delay
