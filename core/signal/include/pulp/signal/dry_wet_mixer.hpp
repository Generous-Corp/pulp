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
    void set_mix(SampleType mix) {
        mix_ = std::clamp(mix, SampleType{0.0f}, SampleType{1.0f});
    }
    SampleType mix() const { return mix_; }

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
    /// audio-thread safe. After prepare(), set_mix(), set_curve(), push_dry(),
    /// mix_wet(), and reset() are allocation-free for num_channels <=
    /// max_channels and num_samples <= max_block_size. Calls that exceed the
    /// prepared channel/block capacity may grow storage and are non-RT.
    void prepare(int max_channels, int max_block_size) {
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
        SampleType dry_gain, wet_gain;
        compute_gains(dry_gain, wet_gain);

        const int channel_count = std::min(num_channels, active_dry_channels_);
        for (int ch = 0; ch < channel_count; ++ch) {
            const int sample_count = std::min(num_samples, active_dry_samples_);
            for (int i = 0; i < sample_count; ++i) {
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
    }

private:
    SampleType mix_ = SampleType{1.0f};
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

    void compute_gains(SampleType& dry, SampleType& wet) const {
        constexpr SampleType kHalfPi = SampleType{1.57079632679489661923f};
        const SampleType theta = mix_ * kHalfPi;
        switch (curve_) {
            case MixCurve::Linear:
                dry = SampleType{1.0f} - mix_;
                wet = mix_;
                break;
            case MixCurve::EqualPower:
            case MixCurve::Sin3dB:
                dry = std::cos(theta);
                wet = std::sin(theta);
                break;
            case MixCurve::Balanced:
                dry = mix_ <= SampleType{0.5f}
                    ? SampleType{1.0f}
                    : (SampleType{1.0f} - mix_) * SampleType{2.0f};
                wet = mix_ >= SampleType{0.5f}
                    ? SampleType{1.0f}
                    : mix_ * SampleType{2.0f};
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
                dry = std::sqrt(SampleType{1.0f} - mix_);
                wet = std::sqrt(mix_);
                break;
            case MixCurve::Sqrt4_5dB:
                // -4.5 dB = geometric mean of -3 dB sqrt (exp 0.5) and
                // -6 dB linear (exp 1.0) → exponent 0.75. At mix=0.5
                // produces 0.5^0.75 ≈ 0.5946 per side ≈ -4.51 dB,
                // matching the documented midpoint notch.
                dry = std::pow(SampleType{1.0f} - mix_, SampleType{0.75f});
                wet = std::pow(mix_, SampleType{0.75f});
                break;
        }
    }
};

using DryWetMixer = DryWetMixerT<float>;
using DryWetMixer64 = DryWetMixerT<double>;

}  // namespace pulp::signal
