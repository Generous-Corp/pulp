#pragma once

// VST3 program-list constants and index arithmetic.
//
// A plug-in that ships factory presets publishes them to a VST3 host as one
// program list. The host picks a program by writing the list's program-change
// parameter, whose normalized value maps linearly onto the program index
// (stepCount = programs - 1). This header holds the pure parts so they can be
// tested without the VST3 SDK, mirroring vst3_midi_mapping.hpp.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::format::detail {

/// ProgramListID of the factory program list.
inline constexpr std::int32_t kVst3FactoryProgramListId = 1;

/// ParamID of the factory list's program-change parameter ("pPrg").
///
/// A private ID well above author-assigned parameter IDs and distinct from the
/// synthesized bypass ID (0x70427970) and the MIDI-controller range
/// (0xC0000000 and up). The adapter skips the program list when a real
/// parameter already uses this ID, so a parameter is never shadowed.
inline constexpr std::uint32_t kVst3ProgramChangeParamId = 0x70507267u;

/// Program index selected by a normalized program-change value.
/// Out-of-range and non-finite input clamps to the first or last program.
/// Returns 0 when there are no programs.
inline std::size_t vst3_program_index_from_normalized(double normalized,
                                                      std::size_t program_count) noexcept {
    if (program_count <= 1 || !std::isfinite(normalized))
        return 0;
    const double clamped = std::clamp(normalized, 0.0, 1.0);
    const double scaled = clamped * static_cast<double>(program_count - 1);
    return std::min(static_cast<std::size_t>(std::llround(scaled)), program_count - 1);
}

/// Normalized program-change value for program @p index.
inline double vst3_normalized_from_program_index(std::size_t index,
                                                 std::size_t program_count) noexcept {
    if (program_count <= 1)
        return 0.0;
    return static_cast<double>(std::min(index, program_count - 1)) /
           static_cast<double>(program_count - 1);
}

} // namespace pulp::format::detail
