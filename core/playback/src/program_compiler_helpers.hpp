#pragma once

#include <pulp/playback/track_mixer_program.hpp>
#include <pulp/timeline/model.hpp>

#include <cstdint>

namespace pulp::playback {
class TrackAutomationProgram;
}

namespace pulp::playback::detail {

std::int64_t ratchet_boundary(std::int64_t duration, std::int64_t index,
                              std::int64_t count) noexcept;
TrackMixerProgram resolve_track_mixer(const timeline::Track& track,
                                      const TrackAutomationProgram* automation);

} // namespace pulp::playback::detail
