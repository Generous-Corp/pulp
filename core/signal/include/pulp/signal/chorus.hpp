#pragma once

#include <pulp/signal/delay_line.hpp>
#include <algorithm>
#include <cmath>

namespace pulp::signal {

// Stereo chorus effect using modulated delay lines.
//
// RT contract: `prepare()` allocates delay-line storage. After preparation,
// setters, `process()`, and `reset()` allocate no memory.
template <typename SampleType = float>
class ChorusT {
public:
    void prepare(SampleType sample_rate) {
        if (!std::isfinite(sample_rate) || sample_rate <= SampleType{0.0f})
            sample_rate = SampleType{44100.0f};

        sample_rate_ = sample_rate;
        int max_delay =
            std::max(1, static_cast<int>(
                sample_rate * max_delay_ms * SampleType{0.001f}));
        delay_l_.prepare(max_delay);
        delay_r_.prepare(max_delay);
        prepared_ = true;
    }

    void set_rate(SampleType hz) {
        rate_ = std::isfinite(hz) ? std::max(SampleType{0.0f}, hz) : SampleType{0.0f};
    } // LFO rate (0.1-5 Hz typical)
    void set_depth(SampleType d) {
        depth_ = std::isfinite(d)
            ? std::clamp(d, SampleType{0.0f}, SampleType{1.0f})
            : SampleType{0.0f};
    } // Modulation depth (0-1)
    void set_mix(SampleType m) {
        mix_ = std::isfinite(m)
            ? std::clamp(m, SampleType{0.0f}, SampleType{1.0f})
            : SampleType{0.0f};
    } // Dry/wet mix (0-1)
    void set_delay_ms(SampleType ms) {
        delay_ms_ = std::isfinite(ms)
            ? std::clamp(ms, SampleType{0.0f}, max_delay_ms)
            : SampleType{0.0f};
    } // Center delay (5-30ms typical)

    struct StereoSample { SampleType left, right; };

    StereoSample process(SampleType input) {
        if (!prepared_)
            return {input, input};

        SampleType lfo_l = std::sin(SampleType{2.0f} * pi * phase_);
        SampleType lfo_r = std::sin(SampleType{2.0f} * pi * phase_ +
                                    pi * SampleType{0.5f}); // 90° offset

        SampleType delay_samples = delay_ms_ * sample_rate_ * SampleType{0.001f};
        SampleType mod_l = delay_samples + lfo_l * depth_ * delay_samples *
            SampleType{0.5f};
        SampleType mod_r = delay_samples + lfo_r * depth_ * delay_samples *
            SampleType{0.5f};

        delay_l_.push(input);
        delay_r_.push(input);

        SampleType wet_l = delay_l_.read(mod_l);
        SampleType wet_r = delay_r_.read(mod_r);

        phase_ += rate_ / sample_rate_;
        if (phase_ >= SampleType{1.0f})
            phase_ = std::fmod(phase_, SampleType{1.0f});

        SampleType dry = SampleType{1.0f} - mix_;
        return {input * dry + wet_l * mix_,
                input * dry + wet_r * mix_};
    }

    void reset() {
        delay_l_.reset();
        delay_r_.reset();
        phase_ = 0;
    }

private:
    static constexpr SampleType pi = SampleType{3.14159265358979323846f};
    static constexpr SampleType max_delay_ms = SampleType{50.0f};
    DelayLineT<SampleType> delay_l_, delay_r_;
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType rate_ = SampleType{1.0f};
    SampleType depth_ = SampleType{0.5f};
    SampleType mix_ = SampleType{0.5f};
    SampleType delay_ms_ = SampleType{15.0f};
    SampleType phase_ = 0;
    bool prepared_ = false;
};

using Chorus = ChorusT<float>;
using Chorus64 = ChorusT<double>;

} // namespace pulp::signal
