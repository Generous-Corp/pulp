#pragma once

#include <pulp/signal/lofi_chain.hpp>
#include <pulp/signal/oversampling_fir.hpp>

#include <algorithm>
#include <array>
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
/// set the level, then apply an optional AHD/VCA.
///
/// The order is not arbitrary and is not left to each voice to remember.
/// Folding runs before saturation because a folder generates partials and the
/// saturator then limits them; the other way round the limiter removes what
/// the folder was for. Saturation before quantisation means the quantiser sees
/// a signal that already fills its range, which is what makes a low bit depth
/// sound like a drum machine rather than like a fault; the reverse order
/// quantises a small signal and then amplifies its error. The output level and
/// AHD are both clean post-nonlinearity gains, so neither changes how hard the
/// voice drives its own distortion.
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
        ahd_triggers_.fill(false);
        ahd_trigger_cursor_ = 0;
        ahd_phase_ = AhdPhase::inactive;
        ahd_gain_ = ahd_enabled_ ? 0.0 : 1.0;
    }

    /// Restarts hit-scoped degradation without discarding samples already
    /// travelling through the linear-phase output filters.
    void reset_nonlinear_state() { lofi_.reset(); }

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

    /// Exact host-rate latency for a quality choice. This is constexpr so a
    /// processor that selects one fixed drum quality can report its latency
    /// without constructing or preparing an output stage.
    static constexpr int latency_samples_for(
        OutputOversampling factor) noexcept {
        switch (factor) {
            case OutputOversampling::bypass: return 0;
            case OutputOversampling::x2: return halfband_latency_samples();
            case OutputOversampling::x4:
                return halfband_latency_samples() +
                       halfband_latency_samples() / 2;
        }
        return 0;
    }

    /// Constant group delay at the host rate. Consumers that place a drum path
    /// beside an undelayed path can report or compensate this exact value.
    int latency_samples() const noexcept {
        return latency_samples_for(oversampling_);
    }

    /// True while the linear-phase pair still owns delayed output after the
    /// source layers have finished. Drum voices include this in their activity
    /// predicate so the final few dozen samples are drained instead of cut.
    bool has_tail() const noexcept {
        return tail_samples_remaining_ > 0 || lofi_.has_tail();
    }

    /// Saturation amount, 0 (clean) to 1. Maps to a pre-gain of 1 to 10 into a
    /// tanh, so the control reaches obvious distortion without the top of its
    /// range being a dead zone.
    void set_drive(double amount) { drive_ = std::clamp(amount, 0.0, 1.0); }

    /// Wavefolding amount, 0 to 1.
    void set_fold(double amount) { fold_ = std::clamp(amount, 0.0, 1.0); }

    /// Output gain, linear.
    void set_level(double level) { level_ = std::max(level, 0.0); }

    /// Configure and enable the dedicated post-saturation attack/hold/decay
    /// VCA. All times are milliseconds. `trigger()` starts it; disabling it is
    /// sample-exact transparent.
    void set_ahd_ms(double attack_ms, double hold_ms, double decay_ms) {
        ahd_attack_ms_ = std::max(attack_ms, 0.0);
        ahd_hold_ms_ = std::max(hold_ms, 0.0);
        ahd_decay_ms_ = std::max(decay_ms, 0.01);
        ahd_enabled_ = true;
        update_ahd_times();
    }

    void set_ahd_enabled(bool enabled) {
        ahd_enabled_ = enabled;
        if (!enabled) {
            ahd_triggers_.fill(false);
            ahd_phase_ = AhdPhase::inactive;
            ahd_gain_ = 1.0;
        }
    }

    bool ahd_enabled() const noexcept { return ahd_enabled_; }
    double ahd_gain() const noexcept { return ahd_gain_; }

    /// Mirror another stage's controls without copying its FIR, lo-fi, tail,
    /// or envelope history. Parallel render paths use this to stay under one
    /// public set of controls while retaining independent DSP state.
    void sync_configuration_from(const OutputStageT& other) {
        if (oversampling_ != other.oversampling_) {
            oversampling_ = other.oversampling_;
            update_internal_rate();
            reset();
        }
        drive_ = other.drive_;
        fold_ = other.fold_;
        level_ = other.level_;
        lofi_.sync_configuration_from(other.lofi_);
        if (ahd_attack_ms_ != other.ahd_attack_ms_ ||
            ahd_hold_ms_ != other.ahd_hold_ms_ ||
            ahd_decay_ms_ != other.ahd_decay_ms_) {
            ahd_attack_ms_ = other.ahd_attack_ms_;
            ahd_hold_ms_ = other.ahd_hold_ms_;
            ahd_decay_ms_ = other.ahd_decay_ms_;
            update_ahd_times();
        }
        if (ahd_enabled_ != other.ahd_enabled_)
            set_ahd_enabled(other.ahd_enabled_);
    }

    /// Start the AHD for a new hit. Every drum voice calls this from note-on.
    void trigger() {
        if (!ahd_enabled_) {
            ahd_triggers_.fill(false);
            ahd_phase_ = AhdPhase::inactive;
            ahd_gain_ = 1.0;
            return;
        }
        const int delay = latency_samples();
        if (delay > 0) {
            const int size = static_cast<int>(ahd_triggers_.size());
            const int arrival = (ahd_trigger_cursor_ + delay) % size;
            ahd_triggers_[static_cast<std::size_t>(arrival)] = true;
            return;
        }
        start_ahd();
    }

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
        const double envelope = process_ahd();
        return static_cast<SampleType>(
            static_cast<double>(processed) * level_ * envelope);
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
        update_ahd_times();
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

    enum class AhdPhase {
        inactive,
        attack,
        hold,
        decay,
    };

    void update_ahd_times() {
        ahd_attack_samples_ =
            static_cast<int>(0.001 * ahd_attack_ms_ * sample_rate_);
        ahd_hold_samples_ =
            static_cast<int>(0.001 * ahd_hold_ms_ * sample_rate_);
        ahd_decay_samples_ =
            std::max(1, static_cast<int>(0.001 * ahd_decay_ms_ * sample_rate_));
        ahd_attack_step_ =
            ahd_attack_samples_ > 0 ? 1.0 / ahd_attack_samples_ : 1.0;
        ahd_decay_multiplier_ =
            std::pow(1e-6, 1.0 / static_cast<double>(ahd_decay_samples_));
    }

    void begin_ahd_decay() {
        ahd_phase_ = AhdPhase::decay;
        ahd_remaining_ = ahd_decay_samples_;
        ahd_gain_ = 1.0;
    }

    void start_ahd() {
        if (ahd_attack_samples_ > 0) {
            ahd_phase_ = AhdPhase::attack;
            ahd_remaining_ = ahd_attack_samples_;
            ahd_gain_ = 0.0;
        } else if (ahd_hold_samples_ > 0) {
            ahd_phase_ = AhdPhase::hold;
            ahd_remaining_ = ahd_hold_samples_;
            ahd_gain_ = 1.0;
        } else {
            begin_ahd_decay();
        }
    }

    double process_ahd() {
        if (!ahd_enabled_) return 1.0;
        if (ahd_triggers_[static_cast<std::size_t>(ahd_trigger_cursor_)]) {
            ahd_triggers_[static_cast<std::size_t>(ahd_trigger_cursor_)] =
                false;
            start_ahd();
        }
        const double current =
            ahd_phase_ == AhdPhase::inactive ? 0.0 : ahd_gain_;
        switch (ahd_phase_) {
            case AhdPhase::inactive:
                break;
            case AhdPhase::attack:
                ahd_gain_ = std::min(ahd_gain_ + ahd_attack_step_, 1.0);
                if (--ahd_remaining_ <= 0) {
                    ahd_gain_ = 1.0;
                    if (ahd_hold_samples_ > 0) {
                        ahd_phase_ = AhdPhase::hold;
                        ahd_remaining_ = ahd_hold_samples_;
                    } else {
                        begin_ahd_decay();
                    }
                }
                break;
            case AhdPhase::hold:
                if (--ahd_remaining_ <= 0) begin_ahd_decay();
                break;
            case AhdPhase::decay:
                ahd_gain_ *= ahd_decay_multiplier_;
                if (--ahd_remaining_ <= 0) {
                    ahd_gain_ = 0.0;
                    ahd_phase_ = AhdPhase::inactive;
                }
                break;
        }
        ahd_trigger_cursor_ =
            (ahd_trigger_cursor_ + 1) %
            static_cast<int>(ahd_triggers_.size());
        return current;
    }

    LofiChainT<SampleType> lofi_;
    detail::LinearPhaseOversamplingStage2x<SampleType> stage_a_;
    detail::LinearPhaseOversamplingStage2x<SampleType> stage_b_;
    double sample_rate_ = 44100.0;
    double drive_ = 0.0;
    double fold_ = 0.0;
    double level_ = 1.0;
    double ahd_attack_ms_ = 0.0;
    double ahd_hold_ms_ = 0.0;
    double ahd_decay_ms_ = 1000.0;
    double ahd_gain_ = 1.0;
    double ahd_attack_step_ = 1.0;
    double ahd_decay_multiplier_ = 1.0;
    int ahd_attack_samples_ = 0;
    int ahd_hold_samples_ = 0;
    int ahd_decay_samples_ = 1;
    int ahd_remaining_ = 0;
    bool ahd_enabled_ = false;
    static constexpr int kMaxLatency =
        latency_samples_for(OutputOversampling::x4);
    std::array<bool, kMaxLatency + 1> ahd_triggers_{};
    int ahd_trigger_cursor_ = 0;
    AhdPhase ahd_phase_ = AhdPhase::inactive;
    OutputOversampling oversampling_ = OutputOversampling::x2;
    std::size_t tail_samples_remaining_ = 0;
    bool nonlinear_activity_this_sample_ = false;
};

using OutputStage = OutputStageT<float>;
using OutputStage64 = OutputStageT<double>;

}  // namespace pulp::signal::drum
