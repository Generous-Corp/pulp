#include <pulp/audio/sample_slot_materializer.hpp>

#include <pulp/audio/realtime_sample_recorder.hpp>
#include <pulp/audio/sample_slot_bank.hpp>

#include <cstddef>

namespace pulp::audio {

bool materialize_completed_recording_to_slot(
    RealtimeSampleRecorder& recorder,
    SampleSlotBank& bank,
    std::uint32_t slot_index,
    std::span<const float*> channel_scratch) noexcept {
    const auto frames = recorder.frames_recorded();
    const auto channels = recorder.num_channels();
    if (recorder.state() != RealtimeSampleRecorderState::Completed ||
        frames == 0 || channels == 0 ||
        channel_scratch.size() < static_cast<std::size_t>(channels)) {
        return false;
    }

    if (bank.slot_num_channels(slot_index) != channels ||
        bank.slot_num_frames(slot_index) != frames ||
        bank.slot_sample_rate(slot_index) != recorder.sample_rate()) {
        return false;
    }

    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        channel_scratch[ch] = recorder.channel_data(ch);
        if (channel_scratch[ch] == nullptr) return false;
    }

    BufferView<const float> source(channel_scratch.data(),
                                   channels,
                                   static_cast<std::size_t>(frames));
    if (!bank.write_slot(slot_index, source, frames) ||
        !bank.complete_slot(slot_index)) {
        return false;
    }

    return recorder.consume_completed_recording();
}

bool publish_completed_recording_to_slot(
    RealtimeSampleRecorder& recorder,
    SampleSlotBank& bank,
    std::span<const float*> channel_scratch,
    std::uint64_t audio_safe_generation) noexcept {
    const auto frames = recorder.frames_recorded();
    const auto channels = recorder.num_channels();
    if (recorder.state() != RealtimeSampleRecorderState::Completed ||
        frames == 0 || channels == 0 ||
        channel_scratch.size() < static_cast<std::size_t>(channels)) {
        return false;
    }

    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        channel_scratch[ch] = recorder.channel_data(ch);
        if (channel_scratch[ch] == nullptr) return false;
    }

    BufferView<const float> source(channel_scratch.data(),
                                   channels,
                                   static_cast<std::size_t>(frames));
    if (!bank.publish_from_buffer(source,
                                  frames,
                                  recorder.sample_rate(),
                                  audio_safe_generation)) {
        return false;
    }

    return recorder.consume_completed_recording();
}

}  // namespace pulp::audio
