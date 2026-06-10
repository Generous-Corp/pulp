#include <pulp/audio/loop_types.hpp>

#include <cmath>

namespace pulp::audio {

LoopValidationResult validate_loop_region(const LoopRegion& region,
                                          std::uint64_t source_frames,
                                          std::uint64_t min_loop_frames) noexcept {
    if (source_frames == 0) {
        return {false, LoopValidationStatus::EmptySource};
    }
    if (!(region.source_sample_rate > 0.0) ||
        !std::isfinite(region.source_sample_rate)) {
        return {false, LoopValidationStatus::InvalidSampleRate};
    }
    if (region.start_frame >= region.end_frame ||
        region.end_frame > source_frames) {
        return {false, LoopValidationStatus::InvalidRange};
    }

    const auto length = region.end_frame - region.start_frame;
    if (length < min_loop_frames) {
        return {false, LoopValidationStatus::TooShort};
    }
    if (region.crossfade_frames > length / 2) {
        return {false, LoopValidationStatus::CrossfadeTooLong};
    }

    return {true, LoopValidationStatus::Ok};
}

}  // namespace pulp::audio
