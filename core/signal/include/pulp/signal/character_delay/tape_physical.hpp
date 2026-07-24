#pragma once

// Physical tape tier — the standard tier's saturation and loss stages replaced
// by the actual physics, plus the wear artifacts a used machine has.
//
//   * Saturation → Jiles-Atherton magnetization (hysteresis.hpp).
//   * Loss stand-in → Wallace's playback-loss physics (Wallace 1951; physical
//     ranges from Bertram 1994), evaluated as a magnitude response and realized
//     as a minimum-phase FIR.
//   * Age → a wear axis: spacing grows, hiss rises, dropouts ("chew") appear,
//     and continuous degradation closes the bandwidth down.
//
// Why the loss filter is a FIR and not the obvious IIR: the three loss terms
// multiply into a response with a genuine null (the gap loss is a sinc, and it
// crosses zero at f = v/g), and no low-order IIR puts a null there and tracks
// it as the tape speed changes. Making it MINIMUM PHASE is what keeps it usable
// in a feedback loop — a linear-phase FIR of this length would add its whole
// group delay to every recirculation, and latency_samples() would stop being 0.
//
// Why the banks are pre-designed: the age macro is a per-sample parameter, and
// designing a filter — an FFT, a cepstral fold, a windowing pass — is not
// something an audio callback may do. So the age axis is sampled at
// kLossFirAgeBanks knots when the sample rate is set, and the audio thread only
// ever interpolates between two neighbouring banks. A tape SPEED change does
// rebuild every bank (that is a control-thread call), and the old coefficients
// stay live and are crossfaded out, never mutated underneath the filter.

#include <pulp/signal/character_delay/hysteresis.hpp>
#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/character_delay/tables.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace pulp::signal::chardelay {

/// Head/tape geometry for the Wallace loss model. All lengths in metres.
struct TapeLossGeometry {
    double speed_ips = 7.5;
    double spacing_m = 5e-6;
    double thickness_m = kTapeCoatingThicknessM;
    double gap_m = kTapeHeadGapM;
};

/// Floor applied to the modeled loss magnitude before the FIR is designed.
/// The physics is unbounded — at 1.875 ips with a worn 25 µm spacing the
/// spacing term passes −400 dB inside the audio band — and asking a 97-tap FIR
/// to realize that is meaningless. Flooring makes the design target well posed
/// and honest: the filter reproduces the loss down to a stated depth and is
/// flat below it. −60 dB is far enough down to be inaudible inside a feedback
/// loop that is itself losing energy every pass.
inline constexpr double kTapeLossFloorDb = -60.0;

/// Wallace playback loss at one frequency: spacing × thickness × gap.
inline double tape_loss_magnitude(double frequency_hz, const TapeLossGeometry& geometry) {
    const double velocity = std::max(geometry.speed_ips * 0.0254, 1e-6);
    const double wavenumber = 2.0 * kPi * std::max(frequency_hz, 0.0) / velocity;

    const double spacing_loss = std::exp(-wavenumber * geometry.spacing_m);

    const double thickness_argument = wavenumber * geometry.thickness_m;
    const double thickness_loss = (thickness_argument < 1e-9)
                                      ? 1.0
                                      : (1.0 - std::exp(-thickness_argument)) / thickness_argument;

    const double gap_argument = wavenumber * geometry.gap_m * 0.5;
    const double gap_loss =
        (gap_argument < 1e-9) ? 1.0 : std::sin(gap_argument) / gap_argument;

    return spacing_loss * thickness_loss * std::abs(gap_loss);
}

/// The same response as the FIR design actually targets, floor included. The
/// acceptance suite compares the realized filter against THIS, not against the
/// unbounded physics.
inline double tape_loss_magnitude_floored(double frequency_hz,
                                          const TapeLossGeometry& geometry) {
    const double floor_linear = std::pow(10.0, kTapeLossFloorDb / 20.0);
    return std::max(tape_loss_magnitude(frequency_hz, geometry), floor_linear);
}

/// Number of FIR taps at a given sample rate: the specified order at 48 kHz,
/// scaled with the rate so the filter spans the same amount of TIME (and so
/// resolves the same lowest feature) on every host.
///
/// RESOLUTION LIMIT — a filter of N taps spans N/fs seconds and cannot express
/// spectral features below roughly fs/N. At 48 kHz and 97 taps that floor is
/// about 500 Hz, which covers every combination the module reaches at 7.5 ips
/// and above (measured worst-case error against the analytic response there:
/// 0.8 dB). It does NOT cover the extreme corner of slowest speed and maximum
/// wear: at 1.875 ips with 25 µm spacing the physics puts the −3 dB point near
/// 94 Hz, and no FIR of the specified order can realize that corner — measured
/// worst-case error 15 dB, and still 2 dB at 513 taps. Lengthening the filter
/// to close that gap would cost more inside the feedback loop than the corner
/// is worth (it is a nearly dead machine either way), so the order stays put
/// and the limit is stated here rather than hidden. Anything that needs the
/// slow/worn corner to be spectrally exact wants a different realization — a
/// cascade that carries the smooth low-frequency tilt in an IIR and leaves the
/// FIR only the gap null — not a longer FIR.
inline std::size_t tape_loss_fir_taps(double fs) {
    const auto scaled = static_cast<std::size_t>(
        std::llround(static_cast<double>(kLossFirOrder48k) * fs / 48000.0));
    return std::clamp<std::size_t>(scaled, 32u, 512u) + 1u;
}

/// Design a minimum-phase FIR realizing the floored loss magnitude.
/// Allocating; control thread only. The cepstral fold is the standard
/// DSP-textbook minimum-phase construction.
inline std::vector<double> design_tape_loss_fir(double fs, std::size_t taps,
                                                const TapeLossGeometry& geometry) {
    int fft_size = 64;
    while (static_cast<std::size_t>(fft_size) < taps * 8u) fft_size <<= 1;
    const auto size = static_cast<std::size_t>(fft_size);

    FftT<double> fft(fft_size);
    std::vector<std::complex<double>> spectrum(size);

    // Log-magnitude of the target response, Hermitian-symmetric.
    for (std::size_t bin = 0; bin <= size / 2; ++bin) {
        const double frequency = static_cast<double>(bin) * fs / static_cast<double>(size);
        const double magnitude = tape_loss_magnitude_floored(frequency, geometry);
        const double log_magnitude = std::log(std::max(magnitude, 1e-12));
        spectrum[bin] = {log_magnitude, 0.0};
        if (bin > 0 && bin < size / 2) spectrum[size - bin] = {log_magnitude, 0.0};
    }

    // Real cepstrum, folded to its causal part: the minimum-phase spectrum is
    // the analytic completion of the log-magnitude, and folding the cepstrum is
    // how you get it without unwrapping a phase.
    fft.inverse(spectrum.data());
    std::vector<std::complex<double>> cepstrum(size, std::complex<double>{0.0, 0.0});
    cepstrum[0] = {spectrum[0].real(), 0.0};
    for (std::size_t n = 1; n < size / 2; ++n) cepstrum[n] = {2.0 * spectrum[n].real(), 0.0};
    cepstrum[size / 2] = {spectrum[size / 2].real(), 0.0};

    fft.forward(cepstrum.data());
    for (auto& value : cepstrum) value = std::exp(value);
    fft.inverse(cepstrum.data());

    // Truncate and window. A minimum-phase impulse response concentrates its
    // energy at the START, which is exactly why truncation is cheap here — and
    // also why the window has to be the trailing HALF of a Kaiser rather than a
    // symmetric one. A symmetric window is centred on the middle tap, so it
    // would attenuate h[0] (where nearly all the energy is) to nothing and
    // amplify the tail; the result is not a taper, it is a different filter.
    std::vector<double> symmetric(2u * taps - 1u, 0.0);
    kaiser_window(symmetric, kaiser_beta_for_stopband(kHysteresisHalfBandStopbandDb));

    std::vector<double> coefficients(taps, 0.0);
    for (std::size_t i = 0; i < taps; ++i)
        coefficients[i] = cepstrum[i].real() * symmetric[taps - 1u + i];

    // Normalize DC gain to the model's DC gain (unity: every loss term is 1 at
    // zero frequency), so windowing cannot introduce a level shift that the
    // feedback loop would then compound.
    double sum = 0.0;
    for (double c : coefficients) sum += c;
    if (std::abs(sum) > 1e-9) {
        for (double& c : coefficients) c /= sum;
    }
    return coefficients;
}

/// The loss-FIR bank set for one tape speed: kLossFirAgeBanks designs across
/// the age axis, flattened into one contiguous buffer.
class TapeLossBanks {
public:
    void design(double fs, double speed_ips) {
        taps_ = tape_loss_fir_taps(fs);
        coefficients_.assign(kLossFirAgeBanks * taps_, 0.0);
        group_delay_.assign(kLossFirAgeBanks, 0.0);
        for (std::size_t bank = 0; bank < kLossFirAgeBanks; ++bank) {
            TapeLossGeometry geometry;
            geometry.speed_ips = speed_ips;
            geometry.spacing_m = spacing_for_bank(bank) * 1e-6;
            const auto design = design_tape_loss_fir(fs, taps_, geometry);
            std::copy(design.begin(), design.end(), coefficients_.begin() + bank * taps_);

            // DC group delay, Σ n·h[n] / Σ h[n]. Minimum phase is not ZERO
            // phase: a heavily lossy design still centres its energy several
            // dozen samples in, and that delay sits inside the feedback loop.
            // Measuring it here is what lets the module fold it out of the line
            // and keep the echo landing where the time knob says.
            double weighted = 0.0;
            double sum = 0.0;
            for (std::size_t i = 0; i < taps_; ++i) {
                const double c = coefficients_[bank * taps_ + i];
                weighted += static_cast<double>(i) * c;
                sum += c;
            }
            group_delay_[bank] = (std::abs(sum) > 1e-12) ? weighted / sum : 0.0;
        }
    }

    static double age_for_bank(std::size_t bank) noexcept {
        return static_cast<double>(bank) / static_cast<double>(kLossFirAgeBanks - 1u);
    }

    static double spacing_for_bank(std::size_t bank) noexcept {
        return interpolate_knots(kAgeAxis, kAgeSpacingUm, age_for_bank(bank));
    }

    std::size_t taps() const noexcept { return taps_; }
    bool empty() const noexcept { return coefficients_.empty(); }

    /// DC group delay in samples at `age`, interpolated the same way the
    /// coefficients are.
    double group_delay_samples(double age) const noexcept {
        if (group_delay_.empty()) return 0.0;
        const double position =
            std::clamp(age, 0.0, 1.0) * static_cast<double>(kLossFirAgeBanks - 1u);
        auto lower = static_cast<std::size_t>(position);
        if (lower >= kLossFirAgeBanks - 1u) lower = kLossFirAgeBanks - 2u;
        const double t = position - static_cast<double>(lower);
        return group_delay_[lower] + t * (group_delay_[lower + 1u] - group_delay_[lower]);
    }

    /// Blend the two banks bracketing `age` into `out` (which must hold taps()
    /// entries). Allocation-free; safe at control rate.
    void blend(double age, double* out) const noexcept {
        const double position =
            std::clamp(age, 0.0, 1.0) * static_cast<double>(kLossFirAgeBanks - 1u);
        auto lower = static_cast<std::size_t>(position);
        if (lower >= kLossFirAgeBanks - 1u) lower = kLossFirAgeBanks - 2u;
        const double t = position - static_cast<double>(lower);
        const double* a = coefficients_.data() + lower * taps_;
        const double* b = a + taps_;
        for (std::size_t i = 0; i < taps_; ++i) out[i] = a[i] + t * (b[i] - a[i]);
    }

private:
    std::vector<double> coefficients_;
    std::vector<double> group_delay_;
    std::size_t taps_ = 0;
};

/// Intermittent dropout. Two states with randomly drawn durations and
/// raised-cosine transitions, so the artifact arrives and leaves the way a
/// crease in the tape passes the head rather than switching on a sample edge.
class TapeChew {
public:
    void prepare(double fs) {
        sample_rate_ = fs;
        transition_samples_ = std::max(1, static_cast<int>(kChewTransitionS * fs));
        lowpass_.set_cutoff(kChewLpHz, fs);
        reset();
    }

    void reset() noexcept {
        rng_.reset();
        lowpass_.reset();
        degraded_ = false;
        envelope_ = 0.0;
        remaining_ = draw_duration(kChewCleanScaleS);
        state_index_ = 0;
    }

    void set_seed(std::uint32_t seed) noexcept { rng_.reseed(seed); }

    /// State-transition count since reset — the acceptance suite's determinism
    /// check compares this sequence across renders.
    std::size_t state_index() const noexcept { return state_index_; }

    double process(double x, double depth) noexcept {
        if (--remaining_ <= 0) {
            degraded_ = !degraded_;
            ++state_index_;
            remaining_ = draw_duration(degraded_ ? kChewDegradedScaleS : kChewCleanScaleS);
        }

        const double target = degraded_ ? 1.0 : 0.0;
        const double increment = 1.0 / static_cast<double>(transition_samples_);
        envelope_ = std::clamp(envelope_ + (target > envelope_ ? increment : -increment), 0.0, 1.0);
        if (depth <= 0.0 || envelope_ <= 0.0) {
            lowpass_.lowpass(x);  // keep the filter's state warm so re-entry is smooth
            return x;
        }

        const double window = 0.5 * (1.0 - std::cos(kPi * envelope_));
        const double magnitude = std::pow(std::abs(x), kChewPower);
        const double shaped = lowpass_.lowpass((x < 0.0 ? -magnitude : magnitude));
        return x + depth * window * (shaped - x);
    }

private:
    int draw_duration(double scale_seconds) noexcept {
        const double r = std::max(rng_.uniform(), 1e-6);
        const double seconds = scale_seconds * std::pow(r, kChewVariance);
        return std::max(1, static_cast<int>(seconds * sample_rate_));
    }

    Xorshift32 rng_{kPrngSeed};
    OnePole lowpass_;
    double sample_rate_ = 48000.0;
    double envelope_ = 0.0;
    int transition_samples_ = 1;
    int remaining_ = 1;
    bool degraded_ = false;
    std::size_t state_index_ = 0;
};

/// The physical tier for one channel.
class TapePhysicalChannel {
public:
    void prepare(double fs) {
        sample_rate_ = fs;
        solver_rate_ = fs * 4.0;  // the oversampler's factor

        const double emphasis_gain = std::pow(10.0, kTapeEmphasisDb / 20.0);
        record_eq_.set(kTapeEmphasisHz, emphasis_gain, fs);
        playback_eq_.set_inverse(kTapeEmphasisHz, emphasis_gain, fs);
        dc_blocker_.set_cutoff(kTapeDcBlockHz, fs);
        hiss_filter_.set_cutoff(kHissLpHz, fs);

        oversampler_.prepare();
        hysteresis_.prepare(solver_rate_);
        chew_.prepare(fs);

        banks_.design(fs, speed_ips_);
        working_.assign(banks_.taps(), 0.0);
        fading_from_.assign(banks_.taps(), 0.0);
        loss_fir_.prepare(banks_.taps());

        crossfade_samples_ = std::max(1, static_cast<int>(kLossBankCrossfadeS * fs));
        degrade_filter_.set_cutoff(20000.0, fs);

        build_makeup_table();
        update(0.0);
        banks_.blend(0.0, working_.data());
        std::copy(working_.begin(), working_.end(), fading_from_.begin());
        reset();
    }

    void reset() noexcept {
        record_eq_.reset();
        playback_eq_.reset();
        dc_blocker_.reset();
        hiss_filter_.reset();
        degrade_filter_.reset();
        head_bump_.reset();
        oversampler_.reset();
        hysteresis_.reset();
        chew_.reset();
        loss_fir_.reset();
        hiss_rng_.reset();
        degrade_rng_.reset();
        crossfade_remaining_ = 0;
    }

    void set_seeds(std::uint32_t base) noexcept {
        hiss_rng_.reseed(base ^ 0x9E3779B9u);
        degrade_rng_.reseed(base ^ 0x85EBCA6Bu);
        chew_.set_seed(base ^ 0xC2B2AE35u);
    }

    /// Control-thread only: redesigns every bank and starts the crossfade.
    void set_speed_ips(double ips) {
        const double clamped = std::clamp(ips, kTapeSpeedsIps.front(), kTapeSpeedsIps.back());
        if (std::abs(clamped - speed_ips_) < 1e-9) return;
        speed_ips_ = clamped;
        // Callers legitimately configure the speed before the sample rate is
        // known (the catalog node does exactly that). Until prepare() has run
        // there are no banks and no working buffer to blend into, so record the
        // speed and let prepare() design against it.
        if (working_.empty()) return;
        std::copy(working_.begin(), working_.end(), fading_from_.begin());
        banks_.design(sample_rate_, speed_ips_);
        banks_.blend(age_, working_.data());
        crossfade_remaining_ = crossfade_samples_;
    }

    double speed_ips() const noexcept { return speed_ips_; }
    const std::vector<double>& coefficients() const noexcept { return working_; }

    /// Control-rate update. In this tier `character_amount` IS the age axis.
    void update(double age) noexcept {
        age_ = std::clamp(age, 0.0, 1.0);

        const double drive = interpolate_knots(kTapeAxis, kTapeDrive, age_);
        const double bias = interpolate_knots(kTapeAxis, kTapeBias, age_);
        hysteresis_.set_character(drive, bias);
        makeup_ = makeup_for(age_);

        hiss_gain_ = std::pow(10.0, interpolate_knots(kAgeAxis, kAgeHissDbfs, age_) / 20.0);
        chew_depth_ = interpolate_knots(kAgeAxis, kAgeChewDepth, age_);
        degrade_ = interpolate_knots(kAgeAxis, kAgeDegrade, age_);

        const double bump_db = interpolate_knots(kTapeAxis, kTapeBumpDb, age_);
        head_bump_.set_bell(interpolate_knots(kHeadBumpIps, kHeadBumpHz, speed_ips_), kTapeBumpQ,
                            bump_db, sample_rate_);

        if (crossfade_remaining_ <= 0) banks_.blend(age_, working_.data());

        // Continuous wear: bandwidth falls exponentially toward the floor, and
        // the level dips slightly — a tired machine is darker AND quieter.
        const double nyquist_guard = 0.45 * sample_rate_;
        const double top = std::min(20000.0, nyquist_guard);
        const double degrade_hz = top * std::pow(kDegradeMinLpHz / top, degrade_);
        degrade_filter_.set_cutoff(degrade_hz, sample_rate_);
        degrade_gain_ = std::pow(10.0, kDegradeGainDipDb * degrade_ / 20.0);
    }

    /// Record EQ → oversampled hysteresis → makeup → playback EQ.
    double pre_process(double x) noexcept {
        const double emphasized = record_eq_.process(x);
        const double magnetized = oversampler_.process(
            emphasized, [this](double sample) { return hysteresis_.process(sample); });
        return playback_eq_.process(magnetized * makeup_);
    }

    /// Wallace loss → head bump → hiss → chew → degrade → DC block.
    double post_process(double x) noexcept {
        loss_fir_.push(x);
        double lossy = loss_fir_.convolve(working_.data(), working_.size());
        if (crossfade_remaining_ > 0) {
            const double t = 1.0 - static_cast<double>(crossfade_remaining_) /
                                       static_cast<double>(crossfade_samples_);
            const double weight = 0.5 * (1.0 - std::cos(kPi * t));
            const double previous = loss_fir_.convolve(fading_from_.data(), fading_from_.size());
            lossy = previous + weight * (lossy - previous);
            if (--crossfade_remaining_ == 0) {
                std::copy(working_.begin(), working_.end(), fading_from_.begin());
            }
        }

        double y = head_bump_.process(lossy);
        y += hiss_filter_.lowpass(hiss_rng_.bipolar()) * hiss_gain_;
        y = chew_.process(y, chew_depth_);
        if (degrade_ > 0.0) {
            y = degrade_filter_.lowpass(y) * degrade_gain_;
            y += degrade_rng_.bipolar() * 1e-4 * degrade_;
        }
        return dc_blocker_.highpass(y);
    }

    /// Group delay the oversampling wrap adds inside the loop, in host samples.
    static int oversampler_latency_samples() noexcept {
        return HalfBandOversampler4x::latency_samples();
    }

    /// Total in-loop delay this tier contributes ahead of the delay line's
    /// read: the oversampler's group delay plus the loss FIR's. The engine
    /// subtracts this from the requested line delay so the echo still lands on
    /// the requested time and latency_samples() can stay 0.
    double in_loop_delay_samples() const noexcept {
        return static_cast<double>(oversampler_latency_samples()) +
               banks_.group_delay_samples(age_);
    }

    const JilesAthertonHysteresis& hysteresis() const noexcept { return hysteresis_; }
    JilesAthertonHysteresis& hysteresis() noexcept { return hysteresis_; }
    std::size_t chew_state_index() const noexcept { return chew_.state_index(); }

private:
    static constexpr std::size_t kMakeupKnots = 11;

    /// Makeup gain is MEASURED, not derived. The Jiles-Atherton small-signal
    /// slope has contributions from both the reversible and irreversible terms
    /// and no closed form worth trusting, so the curve is sampled once at
    /// prepare() by running a low-amplitude tone through a scratch solver at
    /// each knot. That keeps a character sweep from also being a level sweep,
    /// which inside a feedback loop would read as the feedback amount changing.
    void build_makeup_table() {
        for (std::size_t knot = 0; knot < kMakeupKnots; ++knot) {
            const double age =
                static_cast<double>(knot) / static_cast<double>(kMakeupKnots - 1u);
            const double drive = interpolate_knots(kTapeAxis, kTapeDrive, age);
            const double bias = interpolate_knots(kTapeAxis, kTapeBias, age);

            JilesAthertonHysteresis probe;
            probe.prepare(solver_rate_);
            probe.set_character(drive, bias);

            constexpr double kProbeAmplitude = 0.02;
            constexpr double kProbeHz = 1000.0;
            const auto period_samples =
                static_cast<int>(std::llround(solver_rate_ / kProbeHz));
            double input_energy = 0.0;
            double output_energy = 0.0;
            for (int n = 0; n < period_samples * 8; ++n) {
                const double phase = 2.0 * kPi * static_cast<double>(n) / static_cast<double>(period_samples);
                const double in = kProbeAmplitude * std::sin(phase);
                const double out = probe.process(in);
                if (n >= period_samples * 4) {
                    input_energy += in * in;
                    output_energy += out * out;
                }
            }
            const double ratio = (output_energy > 1e-30)
                                     ? std::sqrt(input_energy / output_energy)
                                     : 1.0;
            makeup_table_[knot] = std::clamp(ratio, 0.05, 50.0);
        }
    }

    double makeup_for(double age) const noexcept {
        const double position =
            std::clamp(age, 0.0, 1.0) * static_cast<double>(kMakeupKnots - 1u);
        auto lower = static_cast<std::size_t>(position);
        if (lower >= kMakeupKnots - 1u) lower = kMakeupKnots - 2u;
        const double t = position - static_cast<double>(lower);
        return makeup_table_[lower] + t * (makeup_table_[lower + 1u] - makeup_table_[lower]);
    }

    FirstOrderShelf record_eq_;
    FirstOrderShelf playback_eq_;
    OnePole dc_blocker_;
    OnePole hiss_filter_;
    OnePole degrade_filter_;
    Svf2 head_bump_;
    HalfBandOversampler4x oversampler_;
    JilesAthertonHysteresis hysteresis_;
    TapeChew chew_;
    TapeLossBanks banks_;
    FixedFir loss_fir_;
    Xorshift32 hiss_rng_{kPrngSeed};
    Xorshift32 degrade_rng_{kPrngSeed};

    std::vector<double> working_;
    std::vector<double> fading_from_;
    std::array<double, kMakeupKnots> makeup_table_{};

    double sample_rate_ = 48000.0;
    double solver_rate_ = 192000.0;
    double speed_ips_ = 7.5;
    double age_ = 0.0;
    double makeup_ = 1.0;
    double hiss_gain_ = 0.0;
    double chew_depth_ = 0.0;
    double degrade_ = 0.0;
    double degrade_gain_ = 1.0;
    int crossfade_samples_ = 1;
    int crossfade_remaining_ = 0;
};

}  // namespace pulp::signal::chardelay
