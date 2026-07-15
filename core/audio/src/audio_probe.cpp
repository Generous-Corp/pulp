#include <pulp/audio/audio_probe.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::audio {

void AudioProbe::prepare(int max_channels,
                         int max_frames,
                         double sample_rate,
                         AudioProbeStage stage,
                         CaptureConfig capture) {
    max_channels_ = std::clamp(max_channels, 0, AudioProbeSnapshot::kMaxChannels);
    max_frames_ = max_frames < 0 ? 0 : max_frames;
    sample_rate_ = sample_rate;
    stage_ = stage;

    capture_frames_ = capture.capture_frames < 0 ? 0 : capture.capture_frames;
    capture_storage_channels_ = 0;
    capture_storage_.clear();
    capture_fifo_.reset();
    if (capture_frames_ > 0) {
        // SpscRingIndex reserves one sentinel slot, so allocate capacity+1 so
        // the worst-case queue depth is exactly `capture_frames_`.
        const int cap = capture_frames_ + 1;
        capture_storage_channels_ = std::max(1, max_channels_);
        const auto storage_frames = static_cast<std::size_t>(cap);
        const auto storage_channels =
            static_cast<std::size_t>(capture_storage_channels_);
        capture_storage_.assign(storage_frames * storage_channels, 0.0f);
        capture_fifo_ = std::make_unique<runtime::SpscRingIndex>(cap);
    }

    sequence_number_ = 0;
    clip_count_ = 0;
    nan_inf_count_ = 0;
    clipped_blocks_ = 0;
    nan_blocks_ = 0;
    callbacks_ = 0;
    silence_run_blocks_ = 0;
    dropped_capture_frames_ = 0;

    publish_empty_snapshot();
}

void AudioProbe::analyze_output(const BufferView<const float>& output) noexcept {
    // RT-SAFE BODY. No allocation, no locks, no FFT, no logging, no exceptions,
    // no vector growth. Only bounded scalar arithmetic over the block.
    const int channels =
        std::min<int>(static_cast<int>(output.num_channels()), max_channels_);
    const int frames = static_cast<int>(output.num_samples());

    AudioProbeSnapshot snap{};
    snap.sample_rate = sample_rate_;
    snap.block_size = static_cast<std::uint32_t>(frames < 0 ? 0 : frames);
    snap.channel_count = static_cast<std::uint32_t>(channels < 0 ? 0 : channels);
    snap.stage_id = stage_;

    bool block_silent = true;
    bool block_had_clip = false;       // this block had >=1 clipped sample
    bool block_had_nonfinite = false;  // this block had >=1 NaN/Inf sample

    for (int ch = 0; ch < channels; ++ch) {
        const float* samples = output.channel_ptr(static_cast<std::size_t>(ch));
        float peak = 0.0f;
        double sum_sq = 0.0;
        for (int i = 0; i < frames; ++i) {
            const float x = samples[i];
            // NaN/Inf detection without exceptions or allocation.
            if (!std::isfinite(x)) {
                ++nan_inf_count_;          // per-sample
                block_had_nonfinite = true;
                continue;  // do not fold a non-finite value into peak/RMS
            }
            const float ax = std::fabs(x);
            if (ax > peak) peak = ax;
            if (ax > clip_ceiling_) {
                ++clip_count_;             // per-sample
                block_had_clip = true;
            }
            sum_sq += static_cast<double>(x) * static_cast<double>(x);
        }
        const float rms = frames > 0
            ? static_cast<float>(std::sqrt(sum_sq / static_cast<double>(frames)))
            : 0.0f;
        snap.peak[ch] = peak;
        snap.rms[ch] = rms;
        if (peak > snap.peak_max) snap.peak_max = peak;
        if (rms > snap.rms_max) snap.rms_max = rms;
        if (peak > silence_threshold_) block_silent = false;
    }

    if (channels == 0 || frames == 0) block_silent = true;

    ++callbacks_;
    if (block_had_clip) ++clipped_blocks_;
    if (block_had_nonfinite) ++nan_blocks_;
    if (block_silent) {
        ++silence_run_blocks_;
    } else {
        silence_run_blocks_ = 0;
    }

    // Optional last-N multichannel capture. SPSC: producer (here) only writes;
    // the consumer drains via read_capture(). When the ring is full, frames
    // that do not fit are dropped and counted — never silently lost.
    if (capture_fifo_ && channels > 0 && frames > 0) {
        int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
        capture_fifo_->prepare_to_write(frames, s1, n1, s2, n2);
        const int written = n1 + n2;
        const int cap = capture_frames_ + 1;
        for (int ch = 0; ch < capture_storage_channels_; ++ch) {
            float* dst = capture_storage_.data() +
                static_cast<std::size_t>(ch) * static_cast<std::size_t>(cap);
            const float* src = (ch < channels) ? output.channel_ptr(ch) : nullptr;
            for (int i = 0; i < n1; ++i)
                dst[s1 + i] = src ? src[i] : 0.0f;
            for (int i = 0; i < n2; ++i)
                dst[s2 + i] = src ? src[n1 + i] : 0.0f;
        }
        capture_fifo_->finish_write(written);
        if (written < frames) {
            dropped_capture_frames_ +=
                static_cast<std::uint64_t>(frames - written);
        }
    }

    ++sequence_number_;
    snap.sequence_number = sequence_number_;
    snap.clip_count = clip_count_;
    snap.nan_inf_count = nan_inf_count_;
    snap.clipped_blocks = clipped_blocks_;
    snap.nan_blocks = nan_blocks_;
    snap.callbacks = callbacks_;
    snap.silence_run_blocks = silence_run_blocks_;
    snap.dropped_capture_frames = dropped_capture_frames_;
    snap.overwritten_capture_frames = 0;  // drop-on-full policy: no overwrite

    summary_buf_.write(snap);
}

AudioStats AudioProbe::stats() {
    const AudioProbeSnapshot snap = summary_buf_.read();
    AudioStats out;
    out.callbacks = snap.callbacks;
    out.underruns = 0;  // not tracked by the output-boundary probe itself
    // Per-BLOCK tallies (a block with >=1 clipped/non-finite sample counts
    // once) — NOT the per-sample clip_count / nan_inf_count totals.
    out.clipped_blocks = snap.clipped_blocks;
    out.nan_blocks = snap.nan_blocks;
    // device_xruns / cpu_overloads are MIRRORS owned by the device — the host
    // populates them, the probe never does. Leave at zero here.
    return out;
}

int AudioProbe::read_capture(float* dst, int max_frames) {
    if (!capture_fifo_ || dst == nullptr || max_frames <= 0) return 0;
    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    capture_fifo_->prepare_to_read(max_frames, s1, n1, s2, n2);
    const int total = n1 + n2;
    const float* ch0 = capture_storage_.data();
    for (int i = 0; i < n1; ++i)
        dst[i] = ch0[s1 + i];
    for (int i = 0; i < n2; ++i)
        dst[n1 + i] = ch0[s2 + i];
    capture_fifo_->finish_read(total);
    return total;
}

int AudioProbe::read_capture(BufferView<float> dst, int max_frames) {
    if (!capture_fifo_ || max_frames <= 0 || dst.empty()) return 0;
    const int wanted = std::min(max_frames, static_cast<int>(dst.num_samples()));
    if (wanted <= 0) return 0;

    const int cap = capture_frames_ + 1;
    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    capture_fifo_->prepare_to_read(wanted, s1, n1, s2, n2);
    const int total = n1 + n2;
    for (std::size_t ch = 0; ch < dst.num_channels(); ++ch) {
        float* out = dst.channel_ptr(ch);
        if (static_cast<int>(ch) < capture_storage_channels_) {
            const float* src = capture_storage_.data() +
                ch * static_cast<std::size_t>(cap);
            for (int i = 0; i < n1; ++i)
                out[i] = src[s1 + i];
            for (int i = 0; i < n2; ++i)
                out[n1 + i] = src[s2 + i];
        } else {
            std::fill_n(out, static_cast<std::size_t>(total), 0.0f);
        }
    }
    capture_fifo_->finish_read(total);
    return total;
}

void AudioProbe::reset() noexcept {
    sequence_number_ = 0;
    clip_count_ = 0;
    nan_inf_count_ = 0;
    clipped_blocks_ = 0;
    nan_blocks_ = 0;
    callbacks_ = 0;
    silence_run_blocks_ = 0;
    dropped_capture_frames_ = 0;
    if (capture_fifo_) capture_fifo_->reset();
    publish_empty_snapshot();
}

void AudioProbe::publish_empty_snapshot() noexcept {
    // Publish an empty snapshot so a reader before the first callback, or
    // immediately after reset(), sees coherent identity fields and cleared
    // counters instead of stale triple-buffer data.
    AudioProbeSnapshot snap{};
    snap.sample_rate = sample_rate_;
    snap.channel_count = 0;
    snap.stage_id = stage_;
    snap.sequence_number = 0;
    summary_buf_.write(snap);
}

}  // namespace pulp::audio
