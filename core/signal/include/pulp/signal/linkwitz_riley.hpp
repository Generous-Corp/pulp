#pragma once

#include <pulp/signal/biquad.hpp>

namespace pulp::signal {

// Linkwitz-Riley crossover filter (4th order, -6dB at crossover)
// Provides lowpass and highpass outputs that sum flat.
//
// RT contract: coefficient updates, process, and reset are fixed-state only and
// allocate no memory.
class LinkwitzRiley {
public:
    void set_frequency(float hz, float sample_rate) {
        lp1_.set_coefficients(Biquad::Type::lowpass, hz, 0.707f, sample_rate);
        lp2_.set_coefficients(Biquad::Type::lowpass, hz, 0.707f, sample_rate);
        hp1_.set_coefficients(Biquad::Type::highpass, hz, 0.707f, sample_rate);
        hp2_.set_coefficients(Biquad::Type::highpass, hz, 0.707f, sample_rate);
    }

    struct BandSplit { float low, high; };

    BandSplit process(float input) {
        float low = lp2_.process(lp1_.process(input));
        float high = hp2_.process(hp1_.process(input));
        return {low, high};
    }

    void reset() {
        lp1_.reset(); lp2_.reset();
        hp1_.reset(); hp2_.reset();
    }

private:
    Biquad lp1_, lp2_, hp1_, hp2_;
};

} // namespace pulp::signal
