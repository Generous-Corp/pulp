#pragma once

// Dry/wet mix with optional latency compensation.
// Use to blend original (dry) signal with processed (wet) signal.

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace pulp::signal {

/// Crossfade curve between dry (mix=0) and wet (mix=1).
///
/// Conventional crossfade taxonomy. `Sin3dB` is an alias of
/// `EqualPower` kept for explicit naming. Each curve hits dry=1/wet=0
/// at mix=0 and dry=0/wet=1 at mix=1; they differ in the midpoint
/// behavior:
/// - `Linear`:     -6 dB notch at midpoint, constant amplitude sum.
/// - `EqualPower`: -3 dB notch, constant power sum (sin/cos).
/// - `Balanced`:   dry stays at 1 until mix=0.5 then linearly drops
///                 to 0; wet inverse — useful for "include dry until
///                 midway" balance behavior.
/// - `Sin3dB`:     alias of EqualPower.
/// - `Sin4_5dB`:   sin/cos law shaped by exponent 1.5 → -4.5 dB notch.
/// - `Sin6dB`:     sin/cos law squared → -6 dB notch.
/// - `Sqrt3dB`:    sqrt law → -3 dB notch.
/// - `Sqrt4_5dB`:  sqrt with offset → -4.5 dB notch.
enum class MixCurve {
    Linear,
    EqualPower,
    Balanced,
    Sin3dB,
    Sin4_5dB,
    Sin6dB,
    Sqrt3dB,
    Sqrt4_5dB,
};

template <typename SampleType = float>
class DryWetMixerT {
public:
    /// Set the mix ratio (0.0 = fully dry, 1.0 = fully wet)
    ///
    /// The ratio reaches its target over the configured ramp length
    /// (see set_ramp_samples()). With the default ramp length of 0 the
    /// new ratio applies to the whole of the next block.
    void set_mix(SampleType mix) {
        mix_ = std::clamp(mix, SampleType{0.0f}, SampleType{1.0f});
        if (ramp_samples_ <= 0) {
            current_mix_ = mix_;
            ramp_remaining_ = 0;
            mix_increment_ = SampleType{0.0f};
            return;
        }
        mix_increment_ = (mix_ - current_mix_) /
                         static_cast<SampleType>(ramp_samples_);
        ramp_remaining_ = ramp_samples_;
    }
    /// The ratio most recently requested via set_mix(), which is the
    /// ramp target while a ramp is in flight.
    SampleType mix() const { return mix_; }

    /// The ratio the next sample will actually be mixed at. Equal to
    /// mix() once any ramp has settled.
    SampleType current_mix() const { return current_mix_; }

    /// Length of the crossfade applied when set_mix() changes the ratio.
    ///
    /// A mix control driven by a knob or by host automation steps at
    /// block boundaries; crossfading dry against wet across a stepped
    /// ratio is audible as zipper noise. A ramp of a few milliseconds
    /// removes it.
    ///
    /// 0 (the default) keeps the ratio change instantaneous, so the
    /// gains are computed once per block. A non-zero ramp computes them
    /// per sample until the target is reached, which for the pow()- and
    /// trig-based curves is materially more expensive — hence opt-in
    /// rather than always-on.
    ///
    /// RT contract: allocation-free, safe to call from the audio thread
    /// alongside the other setters. Shortening the ramp mid-flight
    /// rescales the remaining distance rather than jumping.
    void set_ramp_samples(int samples) {
        const int new_ramp = std::max(0, samples);
        if (new_ramp == ramp_samples_) return;
        ramp_samples_ = new_ramp;
        if (ramp_samples_ <= 0) {
            // Dropping the ramp settles immediately rather than leaving a
            // partially-applied ratio in place.
            current_mix_ = mix_;
            ramp_remaining_ = 0;
            mix_increment_ = SampleType{0.0f};
            return;
        }
        if (ramp_remaining_ > 0) {
            ramp_remaining_ = std::min(ramp_remaining_, ramp_samples_);
            mix_increment_ = ramp_remaining_ > 0
                ? (mix_ - current_mix_) /
                      static_cast<SampleType>(ramp_remaining_)
                : SampleType{0.0f};
        }
    }
    int ramp_samples() const { return ramp_samples_; }

    /// Convenience form of set_ramp_samples() in seconds.
    void set_ramp_time(SampleType seconds, SampleType sample_rate) {
        if (!(seconds > SampleType{0.0f}) || !(sample_rate > SampleType{0.0f})) {
            set_ramp_samples(0);
            return;
        }
        set_ramp_samples(static_cast<int>(seconds * sample_rate));
    }

    /// True while the ratio is still travelling toward its target.
    bool is_ramping() const { return ramp_remaining_ > 0; }

    /// Set the mixing curve type
    void set_curve(MixCurve curve) { curve_ = curve; }

    /// Set latency in samples for the wet path (compensates dry path delay)
    void set_wet_latency(int samples) {
        const int new_latency = std::max(0, samples);
        if (new_latency == latency_)
            return;

        latency_ = new_latency;
        delay_pos_ = 0;

        if (latency_ > 0 && max_channels_ > 0)
            delay_buffer_.assign(static_cast<size_t>(latency_ * max_channels_),
                                 SampleType{0.0f});
        else
            delay_buffer_.clear();

        for (auto& ch : dry_buffer_)
            std::fill(ch.begin(), ch.end(), SampleType{0.0f});
    }

    /// Prepare for processing.
    ///
    /// RT contract: prepare() allocates dry/latency storage and is not
    /// audio-thread safe. After prepare(), set_mix(), set_curve(),
    /// set_ramp_samples(), set_ramp_time(), push_dry(), mix_wet(), and reset()
    /// are allocation-free for num_channels <=
    /// max_channels and num_samples <= max_block_size. Calls that exceed the
    /// prepared channel/block capacity may grow storage and are non-RT.
    ///
    /// prepare() settles any in-flight mix ramp: a ramp does not survive a
    /// sample-rate or block-size change.
    void prepare(int max_channels, int max_block_size) {
        settle_ramp();
        max_channels_ = std::max(0, max_channels);
        max_block_size_ = std::max(0, max_block_size);
        delay_pos_ = 0;
        if (latency_ > 0 && max_channels_ > 0)
            delay_buffer_.assign(static_cast<size_t>(latency_ * max_channels_),
                                 SampleType{0.0f});
        else
            delay_buffer_.clear();

        dry_buffer_.resize(static_cast<size_t>(max_channels_));
        for (auto& ch : dry_buffer_)
            ch.assign(static_cast<size_t>(max_block_size_), SampleType{0.0f});
        active_dry_channels_ = 0;
        active_dry_samples_ = 0;
    }

    /// Push dry samples before processing (call before your wet processing)
    void push_dry(const SampleType* const* channels, int num_channels, int num_samples) {
        if (channels == nullptr || num_channels <= 0 || num_samples <= 0) {
            active_dry_channels_ = 0;
            active_dry_samples_ = 0;
            return;
        }

        ensure_dry_storage(num_channels, num_samples);

        if (latency_ <= 0) {
            // No latency compensation — just store the dry signal
            for (int ch = 0; ch < num_channels; ++ch) {
                std::memcpy(dry_buffer_[ch].data(), channels[ch],
                            static_cast<size_t>(num_samples) * sizeof(SampleType));
            }
            active_dry_channels_ = num_channels;
            active_dry_samples_ = num_samples;
            return;
        }

        // With latency compensation — delay the dry signal.
        // Process sample-by-sample (all channels per sample) so the delay
        // position advances once per frame, not once per channel.
        const int count = std::min(num_channels, max_channels_);

        for (int i = 0; i < num_samples; ++i) {
            for (int ch = 0; ch < count; ++ch) {
                size_t idx = static_cast<size_t>(delay_pos_ * max_channels_ + ch);
                dry_buffer_[ch][i] = delay_buffer_[idx];
                delay_buffer_[idx] = channels[ch][i];
            }
            delay_pos_ = (delay_pos_ + 1) % latency_;
        }
        active_dry_channels_ = count;
        active_dry_samples_ = num_samples;
    }

    /// Mix dry and wet signals, writing result to wet_channels (in-place)
    void mix_wet(SampleType* const* wet_channels, int num_channels, int num_samples) {
        const int channel_count = std::min(num_channels, active_dry_channels_);
        const int sample_count = std::min(num_samples, active_dry_samples_);
        if (channel_count <= 0 || sample_count <= 0) return;

        if (ramp_remaining_ <= 0) {
            // Settled: one gain pair for the whole block.
            SampleType dry_gain, wet_gain;
            compute_gains(current_mix_, dry_gain, wet_gain);
            for (int ch = 0; ch < channel_count; ++ch) {
                for (int i = 0; i < sample_count; ++i) {
                    wet_channels[ch][i] = dry_buffer_[ch][i] * dry_gain +
                                          wet_channels[ch][i] * wet_gain;
                }
            }
            return;
        }

        // Ramping: the ratio advances once per frame, so frames are the
        // outer loop and every channel of a frame shares its gain pair.
        for (int i = 0; i < sample_count; ++i) {
            if (ramp_remaining_ > 0) {
                current_mix_ += mix_increment_;
                if (--ramp_remaining_ == 0) current_mix_ = mix_;
            }
            SampleType dry_gain, wet_gain;
            compute_gains(current_mix_, dry_gain, wet_gain);
            for (int ch = 0; ch < channel_count; ++ch) {
                wet_channels[ch][i] = dry_buffer_[ch][i] * dry_gain +
                                      wet_channels[ch][i] * wet_gain;
            }
        }
    }

    void reset() {
        std::fill(delay_buffer_.begin(), delay_buffer_.end(), SampleType{0.0f});
        delay_pos_ = 0;
        for (auto& ch : dry_buffer_)
            std::fill(ch.begin(), ch.end(), SampleType{0.0f});
        settle_ramp();
    }

private:
    SampleType mix_ = SampleType{1.0f};
    SampleType current_mix_ = SampleType{1.0f};
    SampleType mix_increment_ = SampleType{0.0f};
    int ramp_samples_ = 0;
    int ramp_remaining_ = 0;
    MixCurve curve_ = MixCurve::Linear;
    int latency_ = 0;
    int max_channels_ = 2;
    int max_block_size_ = 0;
    int delay_pos_ = 0;
    int active_dry_channels_ = 0;
    int active_dry_samples_ = 0;
    std::vector<SampleType> delay_buffer_;
    std::vector<std::vector<SampleType>> dry_buffer_;

    void ensure_dry_storage(int num_channels, int num_samples) {
        if (num_channels <= static_cast<int>(dry_buffer_.size())) {
            for (int ch = 0; ch < num_channels; ++ch) {
                if (num_samples > static_cast<int>(dry_buffer_[ch].size()))
                    dry_buffer_[ch].resize(static_cast<size_t>(num_samples));
            }
            return;
        }

        const auto old_size = dry_buffer_.size();
        dry_buffer_.resize(static_cast<size_t>(num_channels));
        for (std::size_t ch = old_size; ch < dry_buffer_.size(); ++ch)
            dry_buffer_[ch].resize(static_cast<size_t>(std::max(max_block_size_, num_samples)));
        for (std::size_t ch = 0; ch < old_size; ++ch) {
            if (num_samples > static_cast<int>(dry_buffer_[ch].size()))
                dry_buffer_[ch].resize(static_cast<size_t>(num_samples));
        }
    }

    void settle_ramp() {
        current_mix_ = mix_;
        ramp_remaining_ = 0;
        mix_increment_ = SampleType{0.0f};
    }

    void compute_gains(SampleType ratio, SampleType& dry, SampleType& wet) const {
        constexpr SampleType kHalfPi = SampleType{1.57079632679489661923f};
        const SampleType theta = ratio * kHalfPi;
        switch (curve_) {
            case MixCurve::Linear:
                dry = SampleType{1.0f} - ratio;
                wet = ratio;
                break;
            case MixCurve::EqualPower:
            case MixCurve::Sin3dB:
                dry = std::cos(theta);
                wet = std::sin(theta);
                break;
            case MixCurve::Balanced:
                dry = ratio <= SampleType{0.5f}
                    ? SampleType{1.0f}
                    : (SampleType{1.0f} - ratio) * SampleType{2.0f};
                wet = ratio >= SampleType{0.5f}
                    ? SampleType{1.0f}
                    : ratio * SampleType{2.0f};
                break;
            case MixCurve::Sin4_5dB: {
                // Clamp before pow to guard against tiny negative residue.
                const SampleType c = std::max(SampleType{0.0f}, std::cos(theta));
                const SampleType s = std::max(SampleType{0.0f}, std::sin(theta));
                dry = std::pow(c, SampleType{1.5f});
                wet = std::pow(s, SampleType{1.5f});
                break;
            }
            case MixCurve::Sin6dB:
                dry = std::cos(theta) * std::cos(theta);
                wet = std::sin(theta) * std::sin(theta);
                break;
            case MixCurve::Sqrt3dB:
                dry = std::sqrt(SampleType{1.0f} - ratio);
                wet = std::sqrt(ratio);
                break;
            case MixCurve::Sqrt4_5dB:
                // -4.5 dB = geometric mean of -3 dB sqrt (exp 0.5) and
                // -6 dB linear (exp 1.0) → exponent 0.75. At mix=0.5
                // produces 0.5^0.75 ≈ 0.5946 per side ≈ -4.51 dB,
                // matching the documented midpoint notch.
                dry = std::pow(SampleType{1.0f} - ratio, SampleType{0.75f});
                wet = std::pow(ratio, SampleType{0.75f});
                break;
        }
    }
};

using DryWetMixer = DryWetMixerT<float>;
using DryWetMixer64 = DryWetMixerT<double>;

}  // namespace pulp::signal
