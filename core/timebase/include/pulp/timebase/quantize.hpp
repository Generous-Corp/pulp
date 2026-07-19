#pragma once

#include <algorithm>
#include <cmath>

namespace pulp::timebase {

inline constexpr double kBoundaryEpsilonBeats = 1.0e-9;

inline bool valid_sample_rate(double sample_rate) noexcept {
    return sample_rate > 0.0 && std::isfinite(sample_rate);
}

inline bool valid_tempo(double tempo_bpm) noexcept {
    return tempo_bpm > 0.0 && std::isfinite(tempo_bpm);
}

inline bool valid_grid(double grid_beats) noexcept {
    return grid_beats > 0.0 && std::isfinite(grid_beats);
}

inline double beats_per_bar(int numerator, int denominator) noexcept {
    if (numerator <= 0 || denominator <= 0)
        return 0.0;
    return static_cast<double>(numerator) * (4.0 / static_cast<double>(denominator));
}

inline double frames_to_beats(double frames, double sample_rate, double tempo_bpm) noexcept {
    return (frames / sample_rate) * (tempo_bpm / 60.0);
}

inline double beats_to_frames(double beats, double sample_rate, double tempo_bpm) noexcept {
    return (beats * 60.0 / tempo_bpm) * sample_rate;
}

inline double next_grid_boundary(double position_beats, double grid_beats) noexcept {
    const auto index = std::ceil((position_beats - kBoundaryEpsilonBeats) / grid_beats);
    return index * grid_beats;
}

} // namespace pulp::timebase
