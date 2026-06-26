#pragma once

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/audio_probe.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/format/standalone.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

namespace pulp::format::detail {

// Cap on the capture-to-WAV ring window, shared between the writer (clamp) and
// the start()-time ring sizing so the two can never silently diverge into a
// short read. Matches the scope path's window cap.
constexpr int kMaxCaptureWindowSamples = 16384;

// Drain the standalone output probe's capture ring into a WAV file so the
// already-shipped offline `pulp audio validate summarize|assert` verbs can read
// it. Off the realtime thread (called from the one-shot delayed action, like the
// scope-json dump); the producer side (AudioProbe::analyze_output) stays
// allocation/lock-free. Empty path → no-op.
//
// SCOPE CAVEAT (matches the underlying AudioProbe FIFO): the capture ring is
// drop-on-full, so this holds the EARLIEST ~window samples after the stream
// starts, not a rolling "last N", and `write_wav_file` quantizes to int16. That
// is robust for `summarize`/`assert` (presence / level / clip / NaN), but it is
// the wrong source for steady-state `doctor` (THD/response) and is
// quantization-limited for `compare`.
inline bool write_audio_capture_wav_file(const std::string& path,
                                         audio::AudioProbe& probe,
                                         const StandaloneConfig& config) {
    if (path.empty()) return false;

    // 0 (the config default) means "as much as the ring holds" → the shared cap.
    int requested = config.audio_capture_wav_frames > 0
                        ? config.audio_capture_wav_frames
                        : kMaxCaptureWindowSamples;
    requested = std::clamp(requested, 1, kMaxCaptureWindowSamples);

    const auto snapshot = probe.latest();
    const std::size_t channels =
        std::max<std::size_t>(1, static_cast<std::size_t>(snapshot.channel_count));

    audio::Buffer<float> samples(channels, static_cast<std::size_t>(requested));
    const int frames = probe.read_capture(samples.view(), requested);
    if (frames <= 0) return false;  // nothing captured yet — surface as a failure

    audio::AudioFileData data;
    data.sample_rate =
        static_cast<std::uint32_t>(std::max(0.0, snapshot.sample_rate));
    data.channels.resize(channels);
    const auto view = samples.view();
    for (std::size_t c = 0; c < channels; ++c) {
        const float* src = view.channel_ptr(c);
        data.channels[c].assign(src, src + frames);
    }
    return audio::write_wav_file(path, data);
}

}  // namespace pulp::format::detail
