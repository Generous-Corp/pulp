#pragma once

#include <algorithm>
#include <pulp/signal/delay_line.hpp>
#include <array>
#include <cmath>

namespace pulp::signal {

// Feedback Delay Network (FDN) reverb
// 4-channel FDN with Hadamard mixing matrix.
//
// RT contract: `prepare()` allocates delay-line storage. After preparation,
// setters, `process()`, and `reset()` allocate no memory.
template <typename SampleType = float>
class ReverbT {
public:
    void prepare(SampleType sample_rate) {
        sample_rate_ = sample_rate;
        // Prime delay lengths for maximum density (in samples)
        int delays[] = {1087, 1283, 1481, 1693};
        for (int i = 0; i < 4; ++i) {
            lines_[i].prepare(
                static_cast<int>(delays[i] * sample_rate / SampleType{44100.0f}) + 1);
            delay_samples_[i] =
                static_cast<int>(delays[i] * sample_rate / SampleType{44100.0f});
        }
    }

    void set_decay(SampleType seconds) { decay_ = seconds; }
    void set_damping(SampleType d) {
        damping_ = std::clamp(d, SampleType{0.0f}, SampleType{0.99f});
    }
    void set_mix(SampleType m) { mix_ = m; }

    struct StereoSample { SampleType left, right; };

    StereoSample process(SampleType input) {
        // Read from delay lines
        SampleType s[4];
        for (int i = 0; i < 4; ++i)
            s[i] = lines_[i].read(delay_samples_[i]);

        // Hadamard mixing (unitary, energy preserving)
        SampleType h[4];
        h[0] = SampleType{0.5f} * (s[0] + s[1] + s[2] + s[3]);
        h[1] = SampleType{0.5f} * (s[0] - s[1] + s[2] - s[3]);
        h[2] = SampleType{0.5f} * (s[0] + s[1] - s[2] - s[3]);
        h[3] = SampleType{0.5f} * (s[0] - s[1] - s[2] + s[3]);

        // Feedback with decay and damping
        SampleType feedback = decay_feedback();
        for (int i = 0; i < 4; ++i) {
            // One-pole lowpass for damping
            lp_state_[i] += (h[i] - lp_state_[i]) * (SampleType{1.0f} - damping_);
            lines_[i].push(input + lp_state_[i] * feedback);
        }

        // Stereo output (pick pairs)
        SampleType wet_l = (s[0] + s[2]) * SampleType{0.5f};
        SampleType wet_r = (s[1] + s[3]) * SampleType{0.5f};

        SampleType dry = SampleType{1.0f} - mix_;
        return {input * dry + wet_l * mix_,
                input * dry + wet_r * mix_};
    }

    void reset() {
        for (auto& l : lines_) l.reset();
        lp_state_.fill(0);
    }

private:
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType decay_ = SampleType{2.0f};
    SampleType damping_ = SampleType{0.3f};
    SampleType mix_ = SampleType{0.3f};

    std::array<DelayLineT<SampleType>, 4> lines_;
    std::array<int, 4> delay_samples_{};
    std::array<SampleType, 4> lp_state_{};

    SampleType decay_feedback() const {
        // Average delay time
        SampleType avg_delay = 0;
        for (int i = 0; i < 4; ++i)
            avg_delay += delay_samples_[i];
        avg_delay /= (SampleType{4.0f} * sample_rate_);

        // RT60-based feedback
        if (decay_ <= 0 || avg_delay <= 0) return 0;
        return std::pow(SampleType{10.0f}, SampleType{-3.0f} * avg_delay / decay_);
    }
};

using Reverb = ReverbT<float>;
using Reverb64 = ReverbT<double>;

} // namespace pulp::signal
