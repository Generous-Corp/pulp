#pragma once

/// @file midi_map_scale.hpp
/// The normalized output window a controller sweep is mapped onto.

#include <cstdint>

namespace pulp::state {

/// Normalized [0, 1] output window a CC sweep is mapped onto. `min > max`
/// inverts. Defaults to the full range, i.e. an identity CC → normalized map.
struct MidiMapScale {
    float min = 0.0f;
    float max = 1.0f;

    /// Map a raw 7-bit CC value onto this window. The result is always within
    /// [0, 1] because both endpoints are clamped there on insertion.
    float apply(uint8_t value) const {
        return min + (value / 127.0f) * (max - min);
    }

    /// Map a normalized controller position (0..1, any resolution) onto this
    /// window.
    float apply_normalized(float position) const {
        return min + position * (max - min);
    }

    friend constexpr bool operator==(const MidiMapScale&, const MidiMapScale&) = default;
};

} // namespace pulp::state
