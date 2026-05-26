#pragma once

/// @file source_player.hpp
/// `AudioSourcePlayer` — bridges an `AudioSource` into a host audio
/// callback. Handles `prepare_to_play` / `release_resources` lifecycle
/// transparently when the host's sample rate or block size changes.

#include <pulp/audio/source.hpp>

#include <cstddef>

namespace pulp::audio {

/// Drives an `AudioSource` from a host audio callback. The player
/// re-issues `prepare_to_play` automatically when the host's
/// sample-rate or block-size differs from the last call, so callers
/// don't need to call it manually after a device change.
///
/// Output is multi-channel via `BufferView<float>`; channels are
/// rendered into the buffer at offset 0 (the player does not chase
/// `start_sample`; that's a per-source concern).
class AudioSourcePlayer {
public:
    AudioSourcePlayer() = default;

    /// Bind a source (or nullptr to detach). If a source was already
    /// playing, the previous source's resources are released first.
    void set_source(AudioSource* source) {
        if (source_ == source) return;
        if (source_ && prepared_) source_->release_resources();
        source_ = source;
        prepared_ = false;
        last_sample_rate_ = 0.0;
        last_block_size_ = 0;
    }

    AudioSource* source() const { return source_; }

    /// Overall gain applied after the source's render. Default 1.0.
    void set_gain(float gain) { gain_ = gain; }
    float gain() const { return gain_; }

    /// Call from the host's audio callback. Re-prepares the source
    /// if `sample_rate` or `num_samples` differs from the last call.
    void audio_callback(BufferView<float> out,
                         int num_samples,
                         double sample_rate) {
        if (num_samples <= 0 || out.num_channels() == 0) return;
        if (!source_) {
            fill_silence(out, num_samples);
            return;
        }
        if (!prepared_
            || sample_rate != last_sample_rate_
            || num_samples > last_block_size_) {
            source_->prepare_to_play(num_samples, sample_rate);
            last_sample_rate_ = sample_rate;
            last_block_size_ = num_samples;
            prepared_ = true;
        }
        source_->get_next_audio_block(out, /*start_sample=*/0, num_samples);
        if (gain_ != 1.0f) {
            for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
                float* ptr = out.channel_ptr(ch);
                for (int i = 0; i < num_samples; ++i) ptr[i] *= gain_;
            }
        }
    }

    /// Manually release the source's resources (mirrors what
    /// `set_source(nullptr)` does for the prior source).
    void release() {
        if (source_ && prepared_) source_->release_resources();
        prepared_ = false;
        last_sample_rate_ = 0.0;
        last_block_size_ = 0;
    }

private:
    static void fill_silence(BufferView<float> out, int n) {
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
            float* ptr = out.channel_ptr(ch);
            for (int i = 0; i < n; ++i) ptr[i] = 0.0f;
        }
    }

    AudioSource* source_ = nullptr;
    float gain_ = 1.0f;
    bool prepared_ = false;
    double last_sample_rate_ = 0.0;
    int last_block_size_ = 0;
};

} // namespace pulp::audio
