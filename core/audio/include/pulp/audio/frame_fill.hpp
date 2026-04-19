#pragma once

// Small, platform-independent helpers for handling partial-read input
// buffers in the audio callback. Extracted from the Android Oboe device
// (#244) so the fill math is unit-testable without pulling in Oboe.

#include <cstddef>
#include <cstring>

namespace pulp::audio {

/// Zero-fill the trailing samples of an interleaved input buffer after a
/// short read. The caller has populated [0, frames_read) from the input
/// stream; this routine zeros the range [frames_read, total_frames) so
/// the downstream processor sees a deterministic, full-size buffer.
///
/// buf: interleaved float frames (total_frames × channels samples).
/// frames_read: number of frames actually populated (>= 0, <= total_frames).
/// total_frames: number of frames requested for the callback.
/// channels: interleaving factor (1 = mono, 2 = stereo, ...).
///
/// Safe to call with frames_read == total_frames (no-op) and with
/// frames_read == 0 (zeroes the whole buffer). Does nothing if any
/// argument is non-positive or buf is null.
inline void zero_fill_short_read(float* buf,
                                 int frames_read,
                                 int total_frames,
                                 int channels) noexcept {
    if (buf == nullptr) return;
    if (channels <= 0) return;
    if (total_frames <= 0) return;
    if (frames_read < 0) frames_read = 0;
    if (frames_read > total_frames) frames_read = total_frames;
    const std::size_t samples_read =
        static_cast<std::size_t>(frames_read) * static_cast<std::size_t>(channels);
    const std::size_t samples_total =
        static_cast<std::size_t>(total_frames) * static_cast<std::size_t>(channels);
    if (samples_read >= samples_total) return;
    std::memset(buf + samples_read, 0,
                sizeof(float) * (samples_total - samples_read));
}

} // namespace pulp::audio
