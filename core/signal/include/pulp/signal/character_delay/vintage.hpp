#pragma once

// Vintage Digital character — a modeled converter loop at a reduced internal
// rate, entirely INSIDE the feedback path.
//
// Honesty note: no open paper documents the early-80s rack delays this evokes.
// What IS published is every mechanism used here — band-limited clocked
// conversion, pre/de-emphasis around a quantizer, and low-bit PCM artifacts
// (Välimäki et al. 2008 for the general antiquing model; Lipshitz, Wannamaker &
// Vanderkooy 1992 for why the dither is TPDF at ±1 LSB; IEC 60908 for the 50 µs
// emphasis time constant). Every NUMBER in the character's table is a design
// parameter, and this comment is here so nobody later mistakes them for
// measurements.
//
// The defining behavior is that the whole converter sits in the loop: every
// repeat is re-converted, so quantization grit and band-limiting compound —
// repeat 1 has been through one converter, repeat 10 through ten. A converter
// modeled once at the input would sound like a bitcrusher in front of a clean
// delay, which is a different and much less interesting effect.
//
// Because the internal grid is a real clock, a delay-time change slides the
// read position across it and the buckets repitch — the same glide the BBD
// character gets, for the same physical reason.
//
// latency_samples() stays 0: this chain is all IIR plus a sample-and-hold. Its
// group delay is phase, not a bufferable constant, and there is nothing here to
// report to a host as latency.

#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/character_delay/tables.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::signal::chardelay {

class VintageChannel {
public:
    void prepare(double fs) {
        sample_rate_ = fs;
        // The internal grid can never exceed the host rate; on a low-rate host
        // the top table knots simply collapse onto it.
        const double highest_internal = std::min(kVintageInternalRateHz.front(), fs);
        const auto capacity = static_cast<std::size_t>(
            std::ceil(kMaxDelayMs * 0.001 * highest_internal) + 8.0);
        line_.prepare(capacity);
        update(0.0);
        reset();
    }

    void reset() noexcept {
        line_.reset();
        anti_alias_.reset();
        reconstruction_.reset();
        pre_emphasis_.reset();
        de_emphasis_.reset();
        phase_ = 0.0;
        held_ = 0.0;
        rng_.reset();
    }

    void set_seed(std::uint32_t seed) noexcept { rng_.reseed(seed); }

    /// Control-rate update: pick the "range switch" position (internal rate and
    /// word length) for this character amount and retune the matched
    /// anti-alias / reconstruction pair to it.
    void update(double character_amount) noexcept {
        internal_rate_ =
            std::min(interpolate_knots(kVintageAxis, kVintageInternalRateHz, character_amount),
                     sample_rate_);
        const double bits =
            interpolate_knots(kVintageAxis, kVintageBits, character_amount);
        quantization_step_ = std::pow(2.0, -(bits - 1.0));

        const double band_edge = kVintageAntiAliasFraction * internal_rate_;
        anti_alias_.set_cutoff(band_edge, sample_rate_);
        reconstruction_.set_cutoff(band_edge, sample_rate_);

        const double emphasis_gain = std::pow(10.0, kVintageEmphasisDb / 20.0);
        pre_emphasis_.set(kVintageEmphasisHz, emphasis_gain, sample_rate_);
        de_emphasis_.set_inverse(kVintageEmphasisHz, emphasis_gain, sample_rate_);
    }

    double internal_rate_hz() const noexcept { return internal_rate_; }
    double band_edge_hz() const noexcept { return kVintageAntiAliasFraction * internal_rate_; }

    double process(double x, double delay_seconds) noexcept {
        const double band_limited = pre_emphasis_.process(anti_alias_.process(x));

        // "ADC": hold the host-rate signal onto the internal grid, quantize
        // what lands on a tick, and read the line at the fractional grid
        // position the (slewed) delay time asks for.
        phase_ += internal_rate_ / sample_rate_;
        while (phase_ >= 1.0) {
            phase_ -= 1.0;
            line_.push(quantize(band_limited));
            held_ = line_.read_linear(delay_seconds * internal_rate_);
        }

        // "DAC": the held value through the inverse emphasis and the matching
        // reconstruction filter.
        return reconstruction_.process(de_emphasis_.process(held_));
    }

private:
    /// Midtread quantizer with TPDF dither at ±1 LSB — the published correct
    /// dither for PCM quantization: it decorrelates the error from the signal
    /// so low-level material dissolves into a steady hiss instead of the
    /// signal-dependent crunch an undithered quantizer produces (and which,
    /// re-quantized every repeat, would turn into a growing buzz).
    double quantize(double x) noexcept {
        const double dither = (rng_.uniform() + rng_.uniform() - 1.0) * quantization_step_;
        const double q = std::round((x + dither) / quantization_step_) * quantization_step_;
        return std::clamp(q, -1.0, 1.0 - quantization_step_);
    }

    FractionalDelayLine line_;
    Butterworth6Lowpass anti_alias_;
    Butterworth6Lowpass reconstruction_;
    FirstOrderShelf pre_emphasis_;
    FirstOrderShelf de_emphasis_;
    Xorshift32 rng_{kPrngSeed};

    double sample_rate_ = 48000.0;
    double internal_rate_ = 32000.0;
    double quantization_step_ = 1.0 / 8192.0;
    double phase_ = 0.0;
    double held_ = 0.0;
};

}  // namespace pulp::signal::chardelay
