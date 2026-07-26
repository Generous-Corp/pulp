#pragma once

#include <pulp/signal/lofi_chain.hpp>
#include <pulp/signal/oversampling_fir.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::signal::drum {

/// Realtime quality choices for the shared drum output nonlinearity.
///
/// `x2` is the shipping default. It is the same 65-tap Kaiser half-band pair
/// used by Pulp's character-delay hysteresis path. `x4` cascades a second copy
/// for unusually hard fold/drive settings. `bypass` deliberately preserves
/// host-rate aliasing for period-authentic drum-machine sounds and removes the
/// filter latency entirely.
enum class OutputOversampling {
    bypass = 1,
    x2 = 2,
    x4 = 4,
};

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
/// RT contract: `prepare()` configures FIR storage and may allocate. Setters,
/// `reset()`, and `process()` allocate nothing and take no locks.
template <typename SampleType = float>
class OutputStageT {
public:
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        // This is the established house half-band configuration: 65 taps,
        // Kaiser beta ~= 8 (81.3 dB design), with the upper stage keeping the
        // same absolute passband when it runs at twice stage one's rate.
        stage_a_.configure(kHouseHalfBandStageOnePassband,
                           kHouseHalfBandStopbandDb,
                           kHouseHalfBandTaps);
        stage_b_.configure(kHouseHalfBandStageTwoPassband,
                           kHouseHalfBandStopbandDb,
                           kHouseHalfBandTaps);
        update_internal_rate();
        reset();
    }

    void reset() {
        lofi_.reset();
        stage_a_.reset();
        stage_b_.reset();
        tail_samples_remaining_ = 0;
        nonlinear_activity_this_sample_ = false;
    }

    /// Changes the output quality outside the audio callback. The filter banks
    /// are prepared eagerly, so switching among the three choices does not
    /// allocate, but it resets their histories and the lo-fi clock.
    void set_oversampling(OutputOversampling factor) {
        if (factor == oversampling_) return;
        oversampling_ = factor;
        update_internal_rate();
        reset();
    }

    OutputOversampling oversampling() const noexcept { return oversampling_; }

    /// Constant group delay at the host rate. Consumers that place a drum path
    /// beside an undelayed path can report or compensate this exact value.
    int latency_samples() const noexcept {
        switch (oversampling_) {
            case OutputOversampling::bypass: return 0;
            case OutputOversampling::x2: return halfband_latency_samples();
            case OutputOversampling::x4:
                return halfband_latency_samples() + halfband_latency_samples() / 2;
        }
        return 0;
    }

    /// True while the linear-phase pair still owns delayed output after the
    /// source layers have finished. Drum voices include this in their activity
    /// predicate so the final few dozen samples are drained instead of cut.
    bool has_tail() const noexcept { return tail_samples_remaining_ > 0; }

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
        nonlinear_activity_this_sample_ = false;
        SampleType processed = input;
        switch (oversampling_) {
            case OutputOversampling::bypass:
                processed = process_nonlinear(input);
                break;
            case OutputOversampling::x2: {
                SampleType even{};
                SampleType odd{};
                stage_a_.upsample(input, even, odd);
                processed =
                    stage_a_.downsample(process_nonlinear(even), process_nonlinear(odd));
                break;
            }
            case OutputOversampling::x4: {
                SampleType even{};
                SampleType odd{};
                stage_a_.upsample(input, even, odd);
                processed = stage_a_.downsample(process_inner_x2(even),
                                                process_inner_x2(odd));
                break;
            }
        }

        if (nonlinear_activity_this_sample_ && latency_samples() > 0) {
            tail_samples_remaining_ =
                static_cast<std::size_t>(2 * latency_samples() + 1);
        } else if (tail_samples_remaining_ > 0) {
            --tail_samples_remaining_;
        }
        return static_cast<SampleType>(static_cast<double>(processed) * level_);
    }

private:
    static constexpr int halfband_latency_samples() noexcept {
        return static_cast<int>((kHouseHalfBandTaps - 1u) / 2u);
    }

    int factor_value() const noexcept {
        return static_cast<int>(oversampling_);
    }

    void update_internal_rate() {
        lofi_.set_sample_rate(sample_rate_ * static_cast<double>(factor_value()));
    }

    SampleType process_inner_x2(SampleType input) {
        SampleType even{};
        SampleType odd{};
        stage_b_.upsample(input, even, odd);
        return stage_b_.downsample(process_nonlinear(even), process_nonlinear(odd));
    }

    SampleType process_nonlinear(SampleType input) {
        double x = static_cast<double>(input);
        if (fold_ > 0.0) {
            const double k = 1.0 + fold_ * 4.0;
            x = std::sin(0.5 * 3.14159265358979323846 * k * x);
        }
        if (drive_ > 0.0) {
            x = std::tanh((1.0 + drive_ * 9.0) * x);
        }
        x = static_cast<double>(lofi_.process(static_cast<SampleType>(x)));
        nonlinear_activity_this_sample_ |= std::abs(x) > 1e-20;
        return static_cast<SampleType>(x);
    }

    LofiChainT<SampleType> lofi_;
    detail::LinearPhaseOversamplingStage2x<SampleType> stage_a_;
    detail::LinearPhaseOversamplingStage2x<SampleType> stage_b_;
    double sample_rate_ = 44100.0;
    double drive_ = 0.0;
    double fold_ = 0.0;
    double level_ = 1.0;
    OutputOversampling oversampling_ = OutputOversampling::x2;
    std::size_t tail_samples_remaining_ = 0;
    bool nonlinear_activity_this_sample_ = false;
};

using OutputStage = OutputStageT<float>;
using OutputStage64 = OutputStageT<double>;

}  // namespace pulp::signal::drum
