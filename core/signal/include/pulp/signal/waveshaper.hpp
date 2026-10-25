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
        const SampleType x = input * drive_;
        switch (curve_) {
        case Curve::soft_clip:
            return shape<Curve::soft_clip>(x);
        case Curve::hard_clip:
            return shape<Curve::hard_clip>(x);
        case Curve::tanh_clip:
            return shape<Curve::tanh_clip>(x);
        case Curve::fold:
            return shape<Curve::fold>(x);
        case Curve::sine_fold:
            return shape<Curve::sine_fold>(x);
        }
        return x;
    }

    // Same result as process(sample) on each sample, bit for bit (except that
    // at unity drive a signaling NaN input is not quieted first). The curve
    // is selected once per buffer and the drive held in a register: read
    // through the object inside the loop they would be reloaded every sample,
    // since the compiler cannot prove that an opaque libm call leaves them
    // unchanged, and the curve switch would stay in the loop.
    void process(SampleType* buffer, int num_samples) const {
        switch (curve_) {
        case Curve::soft_clip:
            return run<Curve::soft_clip>(buffer, num_samples, drive_);
        case Curve::hard_clip:
            return run<Curve::hard_clip>(buffer, num_samples, drive_);
        case Curve::tanh_clip:
            return run<Curve::tanh_clip>(buffer, num_samples, drive_);
        case Curve::fold:
            return run<Curve::fold>(buffer, num_samples, drive_);
        case Curve::sine_fold:
            return run<Curve::sine_fold>(buffer, num_samples, drive_);
        }
        for (int i = 0; i < num_samples; ++i)
            buffer[i] *= drive_;
    }

  private:
    // The curve applied to an already-driven sample.
    template <Curve C> static SampleType shape(SampleType x) {
        if constexpr (C == Curve::soft_clip) {
            return x / (SampleType{1.0f} + std::abs(x));
        } else if constexpr (C == Curve::hard_clip) {
            return std::clamp(x, SampleType{-1.0f}, SampleType{1.0f});
        } else if constexpr (C == Curve::tanh_clip) {
            return std::tanh(x);
        } else if constexpr (C == Curve::fold) {
            // Wave folding: fold back at +-1
            while (x > SampleType{1.0f} || x < SampleType{-1.0f}) {
                if (x > SampleType{1.0f})
                    x = SampleType{2.0f} - x;
                if (x < SampleType{-1.0f})
                    x = SampleType{-2.0f} - x;
            }
            return x;
        } else {
            return std::sin(x * SampleType{1.5707963f}); // pi/2
        }
    }

    template <Curve C> static void run(SampleType* buffer, int num_samples, SampleType drive) {
        // Multiplying by exactly 1 changes no value other than quieting a
        // signaling NaN, so unity drive skips it: the multiply otherwise sits
        // on the path into each curve evaluation.
        if (drive == SampleType{1.0f}) {
            for (int i = 0; i < num_samples; ++i)
                buffer[i] = shape<C>(buffer[i]);
            return;
        }
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = shape<C>(buffer[i] * drive);
    }

    Curve curve_ = Curve::tanh_clip;
    SampleType drive_ = SampleType{1.0f};
};

using WaveShaper = WaveShaperT<float>;
using WaveShaper64 = WaveShaperT<double>;

} // namespace pulp::signal
