#pragma once

/// @file freeze_hold.hpp
/// Spectral freeze / infinite hold for phase-vocoder frame groups.
///
/// Captures a rolling window of recent analysis frames (preallocated at
/// prepare) and, when engaged, replaces live analysis frames with a
/// synthetic steady-state hold: per-channel magnitudes averaged over the
/// captured window, per-bin phases advancing at the instantaneous
/// frequency estimated from the captured frames, plus a small bounded
/// random walk that de-periodicizes the hold (spectral freezing per
/// J.-F. Charles, "A Tutorial on Spectral Sound Processing Using Max/MSP
/// and Jitter," CMJ 32(3), 2008; the random-walk de-looping follows the
/// granular-hold contrast family, Roads, *Microsound*, 2001).
///
/// Designed to sit at the HEAD of a vocoder chain (before phase
/// propagation and formant processing): held frames look like live
/// steady-state input, so downstream pitch/formant control keeps working
/// over frozen audio. Per-channel phase offsets are initialized from the
/// latched frame, preserving the spatial image.
///
/// Engage policy (explicit no-mute rule): freezing latches only once the
/// capture window is full; until then live frames pass through and the
/// latch arms. Engage/release crossfade over a fixed number of frames in
/// the spectral domain. Deterministic (xorshift PRNG seeded at prepare);
/// no allocation or locks after prepare().

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace pulp::signal {

template <typename SampleType = float>
class FreezeHoldT {
public:
    struct Config {
        int fft_size = 2048;
        int channels = 1;
        int analysis_hop = 512;
        /// Frames averaged into the hold (rolling capture depth).
        int capture_frames = 8;
        /// Engage/release crossfade length in frames.
        int crossfade_frames = 6;
        /// Random-walk step bound (radians/frame) for de-looping.
        float phase_jitter = 0.015f;
    };

    /// RT contract: prepare() allocates capture/hold storage and is not
    /// audio-thread safe. After prepare(), reset(), set_frozen(), accessors,
    /// and process_group() are allocation-free for the prepared channel/bin
    /// counts.
    void prepare(const Config& config) {
        assert(config.fft_size >= 256 && (config.fft_size & (config.fft_size - 1)) == 0);
        assert(config.channels >= 1 && config.capture_frames >= 2);
        config_ = config;
        num_bins_ = config.fft_size / 2 + 1;

        const auto bins = static_cast<size_t>(num_bins_);
        const auto channels = static_cast<size_t>(config.channels);
        const auto depth = static_cast<size_t>(config.capture_frames);
        capture_mag_.assign(channels * depth * bins, SampleType{0});
        capture_ref_phase_.assign(depth * bins, 0.0);
        held_mag_.assign(channels * bins, SampleType{0});
        held_phase_.assign(channels * bins, 0.0);
        inst_freq_.assign(bins, 0.0);
        reset();
    }

    void reset() {
        std::fill(capture_mag_.begin(), capture_mag_.end(), SampleType{0});
        std::fill(capture_ref_phase_.begin(), capture_ref_phase_.end(), 0.0);
        captured_ = 0;
        capture_pos_ = 0;
        engaged_ = false;
        latched_ = false;
        fade_ = SampleType{0};
        rng_ = 0x9e3779b97f4a7c15ull;
    }

    /// RT-safe engage/release request.
    void set_frozen(bool frozen) { engaged_ = frozen; }
    bool is_engaged() const { return engaged_; }
    /// True once held content is actually playing (capture was full).
    bool is_latched() const { return latched_; }

    /// Run at the head of the vocoder chain for every analysis frame
    /// group. Captures live frames; when engaged and latched, replaces
    /// `frames` with hold content (crossfaded at the edges).
    void process_group(std::complex<SampleType>* const* frames, int channels, int num_bins) {
        assert(num_bins == num_bins_);
        assert(channels == config_.channels);

        if (!latched_)
            capture(frames, channels);

        if (engaged_ && !latched_ && captured_ >= config_.capture_frames)
            latch();

        if (!engaged_ && latched_ && fade_ <= SampleType{0})
            latched_ = false; // release completed last frame

        if (!latched_)
            return; // pass-through (possibly still filling — never mute)

        // Crossfade target: 1 while engaged, 0 while releasing. The fade
        // LAW depends on direction. At ENGAGE the hold starts phase-aligned
        // with the live signal (initialized from the latched frame and
        // advanced only AFTER each mix, so the latch frame plays its
        // captured phases verbatim): the sum is correlated, and linear
        // gains keep it amplitude-flat — equal-power would bump it up to
        // 3 dB. At RELEASE the hold has drifted (instantaneous-frequency
        // estimate error + the deliberate phase random-walk), so hold and
        // live are decorrelated: there a linear fade dips ~3–5 dB mid-fade
        // (measured -5.1 dB on a steady tone — an audible "ding"), and the
        // power-flat equal-power law is correct.
        const SampleType step = SampleType{1} / static_cast<SampleType>(config_.crossfade_frames);
        fade_ = std::clamp(fade_ + (engaged_ ? step : -step), SampleType{0}, SampleType{1});
        SampleType hold_gain, live_gain;
        if (engaged_) {
            hold_gain = fade_;
            live_gain = SampleType{1} - fade_;
        } else {
            const SampleType t = fade_ * static_cast<SampleType>(1.57079632679489662); // fade_ * pi/2
            hold_gain = std::sin(t);
            live_gain = std::cos(t);
        }

        for (int ch = 0; ch < channels; ++ch) {
            const SampleType* mags = held_mag_.data()
                                     + static_cast<size_t>(ch) * num_bins_;
            const double* phases = held_phase_.data()
                                   + static_cast<size_t>(ch) * num_bins_;
            // Per-channel frame energies for the transition normalization
            // below: the gain laws above are only flat on average — a
            // narrowband bin whose hold drifted to anti-phase with the live
            // signal partially cancels under ANY fixed-gain crossfade
            // (measured: a residual -2.7 dB release notch on a pure tone).
            // Renormalizing the mixed frame to the power-interpolated
            // target makes the transition level-flat by construction, for
            // any content and any phase relationship.
            double e_live = 0.0, e_hold = 0.0, e_mix = 0.0;
            for (int k = 0; k < num_bins_; ++k) {
                const auto live = frames[ch][k];
                const auto held = std::polar(mags[k] * hold_gain,
                                             static_cast<SampleType>(phases[k]));
                e_live += static_cast<double>(std::norm(live));
                e_hold += static_cast<double>(mags[k]) * mags[k];
                const auto mixed = live * live_gain + held;
                e_mix += static_cast<double>(std::norm(mixed));
                frames[ch][k] = mixed;
            }
            if (live_gain > SampleType{0}) { // mid-fade only; steady hold is exact
                // Target: endpoint energies interpolated by fade POSITION,
                // not by the gains — gain-derived targets are wrong for one
                // correlation case or the other (a g²-weighted target undid
                // the correlated engage fade by up to -3 dB mid-fade).
                const double target =
                    (1.0 - static_cast<double>(fade_)) * e_live
                    + static_cast<double>(fade_) * e_hold;
                const double scale_sq = target / std::max(e_mix, 1e-12);
                const SampleType scale = static_cast<SampleType>(std::sqrt(
                    std::clamp(scale_sq, 0.0625, 16.0)));
                for (int k = 0; k < num_bins_; ++k) frames[ch][k] *= scale;
            }
        }
        // Advance AFTER mixing: the first held frame must reproduce the
        // latched phases exactly, or the engage fade sums partially
        // cancelling signals (one-hop phase skew ≈ arbitrary per bin —
        // measured as a -4.6 dB dip at engage before this ordering).
        advance_hold_phases();
    }

private:
    void capture(std::complex<SampleType>* const* frames, int channels) {
        const auto bins = static_cast<size_t>(num_bins_);
        const auto depth = static_cast<size_t>(config_.capture_frames);
        for (int ch = 0; ch < channels; ++ch) {
            SampleType* slot = capture_mag_.data()
                               + (static_cast<size_t>(ch) * depth
                                  + static_cast<size_t>(capture_pos_)) * bins;
            for (int k = 0; k < num_bins_; ++k)
                slot[k] = std::abs(frames[ch][k]);
        }
        // Reference phase (channel sum) for instantaneous-frequency
        // estimation at latch time.
        double* ref = capture_ref_phase_.data()
                      + static_cast<size_t>(capture_pos_) * bins;
        for (int k = 0; k < num_bins_; ++k) {
            std::complex<SampleType> sum(SampleType{0}, SampleType{0});
            for (int ch = 0; ch < channels; ++ch) sum += frames[ch][k];
            ref[k] = static_cast<double>(std::arg(sum));
        }
        last_frames_ = frames; // valid only within process_group call
        capture_pos_ = (capture_pos_ + 1) % config_.capture_frames;
        if (captured_ < config_.capture_frames) ++captured_;
    }

    void latch() {
        const auto bins = static_cast<size_t>(num_bins_);
        const auto depth = static_cast<size_t>(config_.capture_frames);

        // Hold magnitudes: average over the captured window.
        for (int ch = 0; ch < config_.channels; ++ch) {
            SampleType* held = held_mag_.data() + static_cast<size_t>(ch) * bins;
            std::fill(held, held + bins, SampleType{0});
            for (size_t f = 0; f < depth; ++f) {
                const SampleType* slot = capture_mag_.data()
                                         + (static_cast<size_t>(ch) * depth + f) * bins;
                for (int k = 0; k < num_bins_; ++k) held[k] += slot[k];
            }
            const SampleType inv = SampleType{1} / static_cast<SampleType>(depth);
            for (int k = 0; k < num_bins_; ++k) held[k] *= inv;
        }

        // Instantaneous frequency from the two most recent captures
        // (heterodyned phase increment over one analysis hop).
        const int newest = (capture_pos_ + config_.capture_frames - 1) % config_.capture_frames;
        const int prev = (capture_pos_ + config_.capture_frames - 2) % config_.capture_frames;
        const double* ph1 = capture_ref_phase_.data() + static_cast<size_t>(newest) * bins;
        const double* ph0 = capture_ref_phase_.data() + static_cast<size_t>(prev) * bins;
        const double ha = static_cast<double>(config_.analysis_hop);
        for (int k = 0; k < num_bins_; ++k) {
            const double omega = two_pi_ * k / config_.fft_size;
            const double delta = princarg(ph1[k] - ph0[k] - omega * ha);
            inst_freq_[static_cast<size_t>(k)] = omega + delta / ha;
        }

        // Initial hold phases from the latched (newest) live frame so the
        // per-channel phase relationships — the image — carry into the hold.
        if (last_frames_ != nullptr) {
            for (int ch = 0; ch < config_.channels; ++ch) {
                double* phases = held_phase_.data() + static_cast<size_t>(ch) * bins;
                for (int k = 0; k < num_bins_; ++k)
                    phases[k] = static_cast<double>(std::arg(last_frames_[ch][k]));
            }
        }
        fade_ = SampleType{0};
        latched_ = true;
    }

    void advance_hold_phases() {
        const double ha = static_cast<double>(config_.analysis_hop);
        const double jitter = static_cast<double>(config_.phase_jitter);
        for (int k = 0; k < num_bins_; ++k) {
            const double advance = inst_freq_[static_cast<size_t>(k)] * ha
                                   + jitter * next_uniform();
            for (int ch = 0; ch < config_.channels; ++ch)
                held_phase_[static_cast<size_t>(ch) * num_bins_
                            + static_cast<size_t>(k)] += advance;
        }
    }

    // xorshift64* — deterministic, allocation-free; uniform in [-1, 1].
    double next_uniform() {
        rng_ ^= rng_ >> 12;
        rng_ ^= rng_ << 25;
        rng_ ^= rng_ >> 27;
        const std::uint64_t r = rng_ * 0x2545f4914f6cdd1dull;
        return static_cast<double>(r >> 11) * (2.0 / 9007199254740992.0) - 1.0;
    }

    static double princarg(double p) {
        return p - two_pi_ * std::round(p / two_pi_);
    }

    static constexpr double two_pi_ = 6.28318530717958647692;

    Config config_;
    int num_bins_ = 0;

    std::vector<SampleType> capture_mag_; // channels * depth * bins
    std::vector<double> capture_ref_phase_; // depth * bins
    std::vector<SampleType> held_mag_;    // channels * bins
    std::vector<double> held_phase_;       // channels * bins
    std::vector<double> inst_freq_;        // bins

    std::complex<SampleType>* const* last_frames_ = nullptr;
    int captured_ = 0;
    int capture_pos_ = 0;
    bool engaged_ = false;
    bool latched_ = false;
    SampleType fade_ = SampleType{0};
    std::uint64_t rng_ = 0;
};

using FreezeHold = FreezeHoldT<float>;
using FreezeHold64 = FreezeHoldT<double>;

} // namespace pulp::signal
