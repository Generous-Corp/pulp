#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#include <pulp/format/detail/standalone_rolling_capture.hpp>

#include <algorithm>
#include <atomic>
#include <vector>

namespace pulp::format::detail {

// Runs on the audio-device callback; keep this path allocation-free.
template <typename AnalyzeOutput>
inline void analyze_standalone_output_tap(
    audio::BufferView<float>& output,
    std::vector<const float*>& output_probe_ptrs,
    AnalyzeOutput&& analyze_output,
    bool rolling_capture_active,
    audio::RollingAudioCaptureBuffer& rolling_capture,
    std::atomic<int>& rolling_capture_channels) {
    const size_t out_ch = output.num_channels();
    for (size_t c = 0; c < out_ch && c < output_probe_ptrs.size(); ++c)
        output_probe_ptrs[c] = output.channel_ptr(c);

    const size_t probe_ch = std::min(out_ch, output_probe_ptrs.size());
    const audio::BufferView<const float> out_view(
        output_probe_ptrs.data(), probe_ch, output.num_samples());
    analyze_output(out_view);

    append_standalone_rolling_capture_output(
        rolling_capture_active, rolling_capture, rolling_capture_channels,
        out_view);
}

}  // namespace pulp::format::detail
