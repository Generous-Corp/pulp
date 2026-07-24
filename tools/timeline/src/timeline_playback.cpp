#include "timeline_agent_internal.hpp"

#include <pulp/playback/audio_renderer.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::tools::timeline::detail {
namespace {

void extend_absolute_frame(std::uint64_t& frames, timebase::SamplePosition start,
                           std::uint64_t count, timebase::RationalRate rate,
                           std::uint32_t sample_rate) {
    if (!rate.valid())
        return;
    const auto end = static_cast<long double>(start.value) + static_cast<long double>(count);
    const auto scaled = end * static_cast<long double>(sample_rate) *
                        static_cast<long double>(rate.denominator) /
                        static_cast<long double>(rate.numerator);
    if (scaled > 0.0L &&
        scaled <= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        frames = std::max(frames, static_cast<std::uint64_t>(std::ceil(scaled)));
}

} // namespace

std::uint64_t render_frame_count(const pulp::timeline::Sequence& sequence,
                                 const timebase::CompiledTempoMap& tempo_map,
                                 const playback::PlaybackProgram& program,
                                 std::uint32_t sample_rate) {
    std::uint64_t frames = 0;
    if (const auto duration = sequence.duration()) {
        const auto samples = tempo_map.ticks_to_samples({duration->value});
        if (samples.value > 0)
            frames = static_cast<std::uint64_t>(samples.value);
    }
    if (const auto duration = sequence.absolute_duration()) {
        extend_absolute_frame(frames, {}, duration->sample_count, duration->sample_rate,
                              sample_rate);
    }
    for (const auto& track : program.tracks()) {
        if (!track->audio_program())
            continue;
        for (const auto& clip : track->audio_program()->clips())
            if (clip.timeline_end() > 0)
                frames = std::max(frames, static_cast<std::uint64_t>(clip.timeline_end()));
    }
    for (const auto& track : sequence.tracks()) {
        if (track.freeze() || track.active_take_lane_id().valid())
            continue;
        for (const auto& clip : track.clips()) {
            if (clip.time_anchor() == pulp::timeline::ClipTimeAnchor::Musical) {
                const auto end = tempo_map.ticks_to_samples(clip.end()).value;
                if (end > 0)
                    frames = std::max(frames, static_cast<std::uint64_t>(end));
            } else {
                extend_absolute_frame(frames, clip.absolute_start(),
                                      clip.absolute_duration_samples(), clip.absolute_sample_rate(),
                                      sample_rate);
            }
        }
    }
    return frames;
}

std::string compile_error_message(const playback::CompileError& error) {
    std::string message =
        "timeline compile error " + std::to_string(static_cast<unsigned>(error.code));
    if (error.item.valid())
        message += " at item " + std::to_string(error.item.value);
    return message;
}

} // namespace pulp::tools::timeline::detail
