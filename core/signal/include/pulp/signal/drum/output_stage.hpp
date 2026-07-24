#pragma once

#include <pulp/signal/lofi_chain.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal::drum {

/// The output stage every percussion voice ends with: fold, saturate, degrade,
/// then set the level.
///
/// The order is not arbitrary and is not left to each voice to remember.
/// Folding runs before saturation because a folder generates partials and the
/// saturator then limits them; the other way round the limiter removes what
/// the folder was for. Saturation before quantisation means the quantiser sees
/// a signal that already fills its range, which is what makes a low bit depth
/// sound like a drum machine rather than like a fault; the reverse order
/// quantises a small signal and then amplifies its error. The output level is
/// applied last so it is a clean gain and does not change how hard the voice
/// drives its own distortion.
///
/// RT contract: `prepare()` and the setters allocate nothing. `process()`
/// allocates nothing and takes no locks.
template <typename SampleType = float>
class OutputStageT {
public:
    void prepare(double sample_rate) {
        lofi_.set_sample_rate(sample_rate);
        reset();
    }

    void reset() { lofi_.reset(); }

    /// Saturation amount, 0 (clean) to 1. Maps to a pre-gain of 1 to 10 into a
    /// tanh, so the control reaches obvious distortion without the top of its
    /// range being a dead zone.
    void set_drive(double amount) { drive_ = std::clamp(amount, 0.0, 1.0); }

    /// Wavefolding amount, 0 to 1.
    void set_fold(double amount) { fold_ = std::clamp(amount, 0.0, 1.0); }

    /// Output gain, linear.
    void set_level(double level) { level_ = std::max(level, 0.0); }

    /// The degradation stages, exposed so a voice can configure bit depth,
    /// hold rate, and dead zone without this class proxying five setters.
    LofiChainT<SampleType>& lofi() { return lofi_; }
    const LofiChainT<SampleType>& lofi() const { return lofi_; }

    SampleType process(SampleType input) {
        double x = static_cast<double>(input);
        if (fold_ > 0.0) {
            const double k = 1.0 + fold_ * 4.0;
            x = std::sin(0.5 * 3.14159265358979323846 * k * x);
        }
        if (drive_ > 0.0) {
            x = std::tanh((1.0 + drive_ * 9.0) * x);
        }
        x = static_cast<double>(lofi_.process(static_cast<SampleType>(x)));
        return static_cast<SampleType>(x * level_);
    }

private:
    LofiChainT<SampleType> lofi_;
    double drive_ = 0.0;
    double fold_ = 0.0;
    double level_ = 1.0;
};

using OutputStage = OutputStageT<float>;
using OutputStage64 = OutputStageT<double>;

}  // namespace pulp::signal::drum
