#pragma once

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#include <pulp/format/detail/standalone_audio_capture_wav.hpp>  // kMaxCaptureWindowSamples
#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

namespace pulp::format::detail {

// Materialize the rolling capture's LAST window to a float WAV — the
// steady-state window `pulp audio validate doctor`/`compare` want, with no int16
// quantization floor (the two limitations of the earliest-window int16 dump in
// standalone_audio_capture_wav.hpp).
//
// Concurrency: this runs off-RT (the one-shot delayed action) while the audio
// thread may still be calling append(). hold_last() freezes a snapshot and flips
// an active-hold flag that makes subsequent append() calls no-ops — but the real
// safety guarantee is in materialize_held(): it revalidates the SeqLock cursor
// before AND after the copy and returns Overwritten if the audio thread advanced
// the ring mid-copy (the hold flag only shrinks that window, it doesn't close the
// start-of-hold race). We surface that rare loss as a logged failure rather than
// a silent empty WAV. Empty path / no channels / nothing captured → no-op.
//
// `max_channels` (0 = all prepared channels) trims the written file to the
// channel count actually delivered by the audio callback, so a device that
// under-delivers vs. the configured output bus does not leave phantom silent
// channels in the WAV (which would read as a spurious channel imbalance in
// `validate doctor`/`compare`). Matches the earliest-window writer, which sizes
// to the probe's observed channel count.
inline bool write_audio_capture_rolling_wav_file(
    const std::string& path,
    audio::RollingAudioCaptureBuffer& rolling,
    const StandaloneConfig& config,
    int max_channels = 0) {
    if (path.empty()) return false;
    std::uint32_t channels = rolling.num_channels();
    if (channels == 0) return false;
    if (max_channels > 0)
        channels = std::min(channels, static_cast<std::uint32_t>(max_channels));

    // 0 (the config default) means "as much as the ring holds" → the shared cap,
    // further bounded by what the ring was actually prepared to hold.
    int requested = config.audio_capture_rolling_frames > 0
                        ? config.audio_capture_rolling_frames
                        : kMaxCaptureWindowSamples;
    requested = std::clamp(requested, 1, kMaxCaptureWindowSamples);
    requested = std::min(requested,
                         static_cast<int>(std::max<std::uint64_t>(
                             1, rolling.capacity_frames())));

    const auto hold = rolling.hold_last(static_cast<std::uint64_t>(requested));
    if (!hold.valid()) return false;
    const std::uint64_t frames = hold.snapshot().frame_count;
    if (frames == 0) return false;  // nothing captured yet

    audio::Buffer<float> samples(channels, static_cast<std::size_t>(frames));
    const auto result = rolling.materialize_held(hold, samples.view());
    if (result.status != audio::RollingAudioCaptureMaterializeStatus::Ok ||
        result.frames_copied == 0) {
        // LCOV_EXCL_START
        // The rare start-of-hold race (an append in flight when the hold began
        // overwrote the snapshot). Surface it rather than exit success with no
        // WAV — the caller can re-run.
        runtime::log_warn(
            "Standalone: rolling capture to {} produced no output (status={}, "
            "frames_copied={}); the ring was overwritten mid-capture — re-run",
            path, static_cast<int>(result.status), result.frames_copied);
        return false;
        // LCOV_EXCL_STOP
    }

    audio::AudioFileData data;
    data.sample_rate = static_cast<std::uint32_t>(std::max(0.0, config.sample_rate));
    data.channels.resize(channels);
    const auto view = samples.view();
    const auto copied = static_cast<std::size_t>(result.frames_copied);
    for (std::uint32_t c = 0; c < channels; ++c) {
        const float* src = view.channel_ptr(c);
        data.channels[c].assign(src, src + copied);
    }
    // Float WAV — preserve the full render below the int16 floor for compare/doctor.
    return audio::write_wav_file(path, data, audio::WavBitDepth::Float32);
}

}  // namespace pulp::format::detail
