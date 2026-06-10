#include <pulp/audio/voice_sum_mixer.hpp>

#include <pulp/runtime/simd.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::audio {

namespace {

std::size_t clamped_frame_count(BufferView<float> destination,
                                std::uint64_t frames) noexcept {
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        frames,
        static_cast<std::uint64_t>(destination.num_samples())));
}

void clear_destination(BufferView<float> destination,
                       std::size_t frames) noexcept {
    for (std::size_t channel = 0; channel < destination.num_channels(); ++channel) {
        pulp::runtime::simd_set(0.0f, destination.channel_ptr(channel), frames);
    }
}

bool input_valid_for_mix(const VoiceSumInput& input,
                         VoiceSumResult& result) noexcept {
    if (!input.active) {
        ++result.inputs_skipped;
        return false;
    }
    if (!std::isfinite(input.gain)) {
        ++result.inputs_skipped;
        ++result.nonfinite_gains;
        return false;
    }
    if (input.gain == 0.0f || input.source.empty()) {
        ++result.inputs_skipped;
        return false;
    }
    return true;
}

void add_channel(const float* source,
                 float gain,
                 float* destination,
                 std::size_t frames) noexcept {
    if (gain == 1.0f) {
        pulp::runtime::simd_add(source, destination, destination, frames);
    } else {
        pulp::runtime::simd_add_scaled(source, gain, destination, frames);
    }
}

bool mix_input(const VoiceSumInput& input,
               BufferView<float> destination,
               std::size_t frames) noexcept {
    const auto source_frames =
        std::min(frames, input.source.num_samples());
    if (source_frames == 0 ||
        input.source.num_channels() == 0 ||
        destination.num_channels() == 0) {
        return false;
    }

    if (input.source.num_channels() == 1) {
        const auto* source = input.source.channel_ptr(0);
        for (std::size_t channel = 0; channel < destination.num_channels(); ++channel) {
            add_channel(source,
                        input.gain,
                        destination.channel_ptr(channel),
                        source_frames);
        }
        return true;
    }

    const auto channels =
        std::min(input.source.num_channels(), destination.num_channels());
    for (std::size_t channel = 0; channel < channels; ++channel) {
        add_channel(input.source.channel_ptr(channel),
                    input.gain,
                    destination.channel_ptr(channel),
                    source_frames);
    }
    return channels > 0;
}

}  // namespace

VoiceSumResult VoiceSumMixer::mix(std::span<const VoiceSumInput> inputs,
                                  BufferView<float> destination,
                                  std::uint64_t frames,
                                  const VoiceSumOptions& options) noexcept {
    VoiceSumResult result;
    const auto frame_count = clamped_frame_count(destination, frames);

    if (!options.accumulate && destination.num_channels() > 0 && frame_count > 0) {
        clear_destination(destination, frame_count);
        result.frames_mixed = frame_count;
    }

    for (const auto& input : inputs) {
        if (!input_valid_for_mix(input, result)) continue;
        if (destination.num_channels() == 0 || frame_count == 0) {
            ++result.inputs_skipped;
            continue;
        }

        const auto before = result.frames_mixed;
        if (mix_input(input, destination, frame_count)) {
            ++result.inputs_mixed;
            result.frames_mixed = std::max<std::uint64_t>(
                before,
                std::min<std::uint64_t>(
                    frame_count,
                    static_cast<std::uint64_t>(input.source.num_samples())));
        } else {
            ++result.inputs_skipped;
        }
    }

    return result;
}

}  // namespace pulp::audio
