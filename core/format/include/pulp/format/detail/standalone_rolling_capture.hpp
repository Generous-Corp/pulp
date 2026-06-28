#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#include <pulp/format/detail/standalone_audio_capture_wav.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace pulp::format::detail {

inline bool prepare_standalone_rolling_capture(
    audio::RollingAudioCaptureBuffer& rolling,
    const StandaloneConfig& config) {
    if (config.audio_capture_rolling_path.empty()) return false;

    const int rolling_frames = config.audio_capture_rolling_frames > 0
        ? std::clamp(config.audio_capture_rolling_frames, 1,
                     kMaxCaptureWindowSamples)
        : kMaxCaptureWindowSamples;
    if (config.audio_capture_rolling_frames > kMaxCaptureWindowSamples) {
        runtime::log_info(
            "Standalone: --audio-capture-rolling-frames {} exceeds the {}-sample cap; clamping",
            config.audio_capture_rolling_frames, kMaxCaptureWindowSamples);
    }

    audio::RollingAudioCaptureBufferConfig rolling_config;
    rolling_config.num_channels =
        static_cast<std::uint32_t>(std::max(config.output_channels, 0));
    rolling_config.max_frames = static_cast<std::uint64_t>(rolling_frames);
    return rolling.prepare(rolling_config);
}

inline void append_standalone_rolling_capture_output(
    bool active,
    audio::RollingAudioCaptureBuffer& rolling,
    std::atomic<int>& delivered_channels,
    audio::BufferView<const float> output) noexcept {
    if (!active) return;
    rolling.append(output, output.num_samples());
    delivered_channels.store(static_cast<int>(output.num_channels()),
                             std::memory_order_relaxed);
}

}  // namespace pulp::format::detail
