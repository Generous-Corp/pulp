#pragma once

/// @file audio_probe.hpp
/// Realtime-safe audio output probe. `AudioProbe::analyze_output()` is callable
/// from the audio callback and satisfies a strict RT ABI: no allocation, no
/// std::vector growth, no locks, no file I/O, no logging, no exceptions, no
/// UI/host/package callbacks, and no FFT/STFT. It performs only bounded
/// per-block scalar work (peak, RMS accumulate, clip count, NaN/Inf count,
/// silence-run) and publishes the latest summary through a
/// `runtime::TripleBuffer<AudioProbeSnapshot>`.
///
/// Do not model this on `pulp::view::VisualizationBridge`: that path runs STFT
/// and returns std::vector copies inside the callback, so it is not RT-safe.
/// The RT-safe scalar contract lives here.
///
/// Gating: the whole probe lives behind `PULP_ENABLE_AUDIO_PROBES`. The
/// release-safe counter subset (`AudioStats`) is always available without this
/// flag. When the flag is OFF, this header still compiles (it is just not
/// instantiated by any runtime), but probe *call sites* in runtime code are
/// `#if PULP_ENABLE_AUDIO_PROBES`-guarded so an OFF build pays nothing.

#include <pulp/audio/audio_probe_snapshot.hpp>
#include <pulp/audio/audio_stats.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/runtime/spsc_ring_index.hpp>
#include <pulp/runtime/triple_buffer.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::audio {

/// Configuration for the optional last-N-frames capture ring. When
/// `capture_frames == 0` (default) no ring is allocated and the probe only
/// publishes the latest scalar summary. Defined at namespace scope so it can
/// serve as a default argument to `AudioProbe::prepare()` (a nested type's
/// default member initializer is not yet usable while the enclosing class is
/// still incomplete).
struct AudioProbeCaptureConfig {
    /// Frames-per-channel of history to retain. 0 disables capture.
    int capture_frames = 0;
};

/// Realtime output-boundary probe. Single-producer (audio thread) / single
/// consumer (UI/worker thread). Sizing and all allocation happen exactly once,
/// in `prepare()`. After that, `analyze_output()` is allocation-free.
///
/// Lifetime: `prepare()` must run on a non-audio thread before the audio
/// callback starts calling `analyze_output()`. Re-preparing while the audio
/// thread is live is a programming error (it reallocates).
class AudioProbe {
public:
    AudioProbe() = default;

    /// @copydoc AudioProbeCaptureConfig
    using CaptureConfig = AudioProbeCaptureConfig;

    /// Allocate fixed-capacity storage. THE ONLY place the probe allocates.
    ///
    /// @param max_channels  Upper bound on channels per block. Clamped to
    ///                       `AudioProbeSnapshot::kMaxChannels`.
    /// @param max_frames    Upper bound on frames per block (informational /
    ///                       used to size the capture ring against). Must be > 0
    ///                       for capture to function.
    /// @param sample_rate   Stamped onto every published snapshot.
    /// @param stage         The boundary this probe observes.
    /// @param capture       Optional last-N-frames capture configuration.
    void prepare(int max_channels,
                 int max_frames,
                 double sample_rate,
                 AudioProbeStage stage = AudioProbeStage::kStandaloneOutputBoundary,
                 CaptureConfig capture = {});

    /// Realtime-safe per-block analysis. Callable from the audio callback.
    ///
    /// Performs only bounded scalar work over `output` and publishes an updated
    /// `AudioProbeSnapshot`. Allocation-free, lock-free, no FFT. Channels beyond
    /// the prepared capacity are ignored (clamped).
    void analyze_output(const BufferView<const float>& output) noexcept;

    /// Read the latest published snapshot (UI/worker thread). Returns a copy so
    /// the reader is not racing the TripleBuffer's front buffer.
    AudioProbeSnapshot latest() { return summary_buf_.read(); }

    /// Release-safe counter subset, derived from the latest snapshot. The
    /// `device_*` mirror fields are left at zero here — the HOST populates them
    /// from `AudioDeviceManager` (ownership boundary, see audio_stats.hpp).
    AudioStats stats();

    /// Copy up to `max_frames` of the most recent captured channel-0 history
    /// into `dst` (UI/worker thread). Returns the number of frames written.
    /// Only meaningful when capture was enabled at prepare(). Non-RT; intended
    /// for the consumer side. This is the single-channel convenience reader
    /// over the first channel of the multichannel capture ring.
    int read_capture(float* dst, int max_frames);

    /// Copy up to `max_frames` of the most recent captured multichannel history
    /// into `dst` (UI/worker thread). Missing source channels are zero-filled.
    /// Returns the number of frames written. Single-consumer: this drains the
    /// same FIFO as `read_capture(float*)`.
    int read_capture(BufferView<float> dst, int max_frames);

    /// Reset all cumulative counters and the capture ring. Not thread-safe
    /// against a live audio thread; call when playback is stopped.
    void reset() noexcept;

    int max_channels() const noexcept { return max_channels_; }
    int max_frames() const noexcept { return max_frames_; }
    bool capture_enabled() const noexcept { return capture_frames_ > 0; }

    /// Silence threshold (linear). A block whose per-channel peak is at or below
    /// this is "silent" for the silence-run counter. Default ~ -90 dBFS.
    void set_silence_threshold(float linear) noexcept { silence_threshold_ = linear; }
    /// Clip ceiling (linear). Samples with |x| strictly greater are "clipped".
    void set_clip_ceiling(float linear) noexcept { clip_ceiling_ = linear; }

private:
    void publish_empty_snapshot() noexcept;

    int max_channels_ = 0;
    int max_frames_ = 0;
    int capture_frames_ = 0;
    double sample_rate_ = 0.0;
    AudioProbeStage stage_ = AudioProbeStage::kUnknown;

    float silence_threshold_ = 3.1622776e-5f;  // ~ -90 dBFS
    float clip_ceiling_ = 1.0f;

    std::uint64_t sequence_number_ = 0;
    std::uint64_t clip_count_ = 0;      // per-sample
    std::uint64_t nan_inf_count_ = 0;   // per-sample
    std::uint64_t clipped_blocks_ = 0;  // per-block: blocks with >=1 clip
    std::uint64_t nan_blocks_ = 0;      // per-block: blocks with >=1 NaN/Inf
    std::uint64_t callbacks_ = 0;
    std::uint64_t silence_run_blocks_ = 0;
    std::uint64_t dropped_capture_frames_ = 0;

    runtime::TripleBuffer<AudioProbeSnapshot> summary_buf_;

    // Optional last-N capture: SpscRingIndex over preallocated, probe-owned
    // storage. The FIFO indexes frames; storage is channel-major with
    // `capture_storage_channels_` lanes, each `capture_frames_ + 1` samples
    // long. The ring is sized at prepare(); analyze_output() never resizes it.
    std::vector<float> capture_storage_;
    int capture_storage_channels_ = 0;
    std::unique_ptr<runtime::SpscRingIndex> capture_fifo_;
};

}  // namespace pulp::audio
