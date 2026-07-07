#pragma once

#include <cmath>
#include <algorithm>

namespace pulp::signal {

// Waveshaping distortion with multiple curve types.
// RT contract: setters and process paths are scalar-only and allocate no
// memory.
template <typename SampleType = float>
class WaveShaperT {
public:
    enum class Curve { soft_clip, hard_clip, tanh_clip, fold, sine_fold };

    void set_curve(Curve c) { curve_ = c; }
    void set_drive(SampleType drive) { drive_ = drive; }

    SampleType process(SampleType input) const {
        SampleType x = input * drive_;
        switch (curve_) {
            case Curve::soft_clip:
                return x / (SampleType{1.0f} + std::abs(x));
            case Curve::hard_clip:
                return std::clamp(x, SampleType{-1.0f}, SampleType{1.0f});
            case Curve::tanh_clip:
                return std::tanh(x);
            case Curve::fold:
                // Wave folding: fold back at +-1
                while (x > SampleType{1.0f} || x < SampleType{-1.0f}) {
                    if (x > SampleType{1.0f}) x = SampleType{2.0f} - x;
                    if (x < SampleType{-1.0f}) x = SampleType{-2.0f} - x;
                }
                return x;
            case Curve::sine_fold:
                return std::sin(x * SampleType{1.5707963f}); // pi/2
        }
        return x;
    }

    void process(SampleType* buffer, int num_samples) const {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

private:
    Curve curve_ = Curve::tanh_clip;
    SampleType drive_ = SampleType{1.0f};
};

using WaveShaper = WaveShaperT<float>;
using WaveShaper64 = WaveShaperT<double>;

} // namespace pulp::signal
