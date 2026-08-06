#include "program_compiler_helpers.hpp"

#include <pulp/playback/track_automation_program.hpp>

#include <variant>

namespace pulp::playback::detail {

std::int64_t ratchet_boundary(std::int64_t duration, std::int64_t index,
                              std::int64_t count) noexcept {
    const auto quotient = duration / count;
    const auto remainder = duration % count;
    return quotient * index + (remainder * index) / count;
}

TrackMixerProgram resolve_track_mixer(const timeline::Track& track,
                                      const TrackAutomationProgram* automation) {
    TrackMixerProgram mixer;
    mixer.gain_linear = track.mixer().gain_linear;
    mixer.pan = track.mixer().pan;
    if (!automation)
        return mixer;
    for (const auto& program : automation->programs()) {
        const auto* target = std::get_if<timeline::TrackMixerTarget>(&program->target());
        if (!target)
            continue;
        switch (target->parameter) {
        case timeline::TrackMixerParameter::Gain:
            mixer.gain_automation = program.get();
            break;
        case timeline::TrackMixerParameter::Pan:
            mixer.pan_automation = program.get();
            break;
        }
    }
    return mixer;
}

} // namespace pulp::playback::detail
