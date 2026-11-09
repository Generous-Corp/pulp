#pragma once

/// @file midi_mapping.hpp
/// One controller-to-parameter mapping, shared by the learn engine and its
/// persistence and undo layers.

#include <cstddef>
#include <cstdint>
#include <pulp/state/midi_decode.hpp>
#include <pulp/state/midi_learn.hpp>
#include <pulp/state/midi_map_scale.hpp>
#include <pulp/state/midi_source.hpp>
#include <pulp/state/parameter.hpp>

namespace pulp::state {

inline constexpr std::size_t kMaxMidiMappings = 256;

/// One controller-to-parameter mapping.
struct MidiMapping {
    uint32_t uid = 0; ///< Stable identity; assigned by the engine when 0.
    MidiSource source{};
    ParamID id = 0;
    MidiMapScale scale{};                       ///< Normalized window; min > max inverts.
    RelativeMode mode = RelativeMode::absolute; ///< Applies to cc7 sources.
    RelativeConfig relative{};
    TakeoverMode takeover = TakeoverMode::jump;
    bool feedback = false; ///< Reserved for MIDI-out feedback.

    friend constexpr bool operator==(const MidiMapping&, const MidiMapping&) = default;
};

} // namespace pulp::state
