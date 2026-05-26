#pragma once

/// @file transport_source.hpp
/// `AudioTransportSource` — play / stop + sample-accurate gain ramp
/// around any `PositionableAudioSource`.

#include <pulp/audio/source.hpp>

#include <algorithm>
#include <cstdint>

namespace pulp::audio {

/// Wraps a `PositionableAudioSource` with transport-level control:
///   - `start` / `stop` toggle whether `get_next_audio_block` emits
///     source audio or silence.
///   - `set_gain` applies a linear gain ramp across the next block
///     (no per-sample click on level changes).
///   - position / length / looping pass through to the wrapped
///     source.
///
/// The transport does not own the source — caller is responsible for
/// keeping the `PositionableAudioSource*` alive at least as long as
/// `get_next_audio_block` may be called.
class AudioTransportSource : public PositionableAudioSource {
public:
    AudioTransportSource() = default;

    /// Bind a source (or nullptr to detach). Resets gain ramp state.
    void set_source(PositionableAudioSource* source) {
        source_ = source;
        playing_ = false;
        current_gain_ = target_gain_;
    }

    PositionableAudioSource* source() const { return source_; }

    // ── Transport ──────────────────────────────────────────────────────────

    void start() { playing_ = (source_ != nullptr); }
    void stop()  { playing_ = false; }
    bool is_playing() const { return playing_; }

    /// Set the target gain. The change is applied as a per-sample
    /// ramp from the current gain to `new_gain` across the next
    /// block — click-free under typical fader motion.
    void set_gain(float new_gain) { target_gain_ = new_gain; }
    float gain() const { return target_gain_; }
    float current_gain() const { return current_gain_; }

    // ── AudioSource ────────────────────────────────────────────────────────

    void prepare_to_play(int samples_per_block, double sample_rate) override {
        samples_per_block_ = samples_per_block;
        sample_rate_ = sample_rate;
        if (source_) source_->prepare_to_play(samples_per_block, sample_rate);
        current_gain_ = target_gain_;
    }

    void release_resources() override {
        if (source_) source_->release_resources();
    }

    void get_next_audio_block(BufferView<float> out,
                               int start_sample,
                               int num_samples) override {
        if (num_samples <= 0 || out.num_channels() == 0) return;

        // Stopped or no source → silence + still advance the gain
        // ramp toward target so resuming doesn't snap.
        if (!playing_ || !source_) {
            fill_silence(out, start_sample, num_samples);
            current_gain_ = target_gain_;
            return;
        }

        source_->get_next_audio_block(out, start_sample, num_samples);

        // Apply per-sample ramp from current_gain_ → target_gain_.
        const float total_step = target_gain_ - current_gain_;
        if (total_step == 0.0f) {
            // Constant scale — fast path.
            if (current_gain_ != 1.0f) {
                scale_block(out, start_sample, num_samples, current_gain_);
            }
            return;
        }
        const float step = total_step / static_cast<float>(num_samples);
        float g = current_gain_;
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
            float* ptr = out.channel_ptr(ch) + start_sample;
            float local_g = g;
            for (int i = 0; i < num_samples; ++i) {
                ptr[i] *= local_g;
                local_g += step;
            }
        }
        current_gain_ = target_gain_;
    }

    // ── PositionableAudioSource pass-through ───────────────────────────────

    void set_next_read_position(uint64_t new_position) override {
        if (source_) source_->set_next_read_position(new_position);
    }
    uint64_t get_next_read_position() const override {
        return source_ ? source_->get_next_read_position() : 0;
    }
    uint64_t get_total_length() const override {
        return source_ ? source_->get_total_length() : 0;
    }
    bool is_looping() const override {
        return source_ ? source_->is_looping() : false;
    }
    void set_looping(bool should_loop) override {
        if (source_) source_->set_looping(should_loop);
    }

private:
    static void fill_silence(BufferView<float> out, int start, int count) {
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
            float* ptr = out.channel_ptr(ch) + start;
            for (int i = 0; i < count; ++i) ptr[i] = 0.0f;
        }
    }
    static void scale_block(BufferView<float> out, int start, int count, float g) {
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
            float* ptr = out.channel_ptr(ch) + start;
            for (int i = 0; i < count; ++i) ptr[i] *= g;
        }
    }

    PositionableAudioSource* source_ = nullptr;
    bool playing_ = false;
    float current_gain_ = 1.0f;
    float target_gain_ = 1.0f;
    int samples_per_block_ = 0;
    double sample_rate_ = 0.0;
};

} // namespace pulp::audio
