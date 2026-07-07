#pragma once

#include <pulp/signal/biquad.hpp>

namespace pulp::signal {

// Linkwitz-Riley crossover filter (4th order, -6dB at crossover)
// Provides lowpass and highpass outputs that sum flat.
//
// RT contract: coefficient updates, process, and reset are fixed-state only and
// allocate no memory.
template <typename SampleType = float>
class LinkwitzRileyT {
public:
    void set_frequency(SampleType hz, SampleType sample_rate) {
        lp1_.set_coefficients(BiquadT<SampleType>::Type::lowpass, hz,
                              SampleType{0.707f}, sample_rate);
        lp2_.set_coefficients(BiquadT<SampleType>::Type::lowpass, hz,
                              SampleType{0.707f}, sample_rate);
        hp1_.set_coefficients(BiquadT<SampleType>::Type::highpass, hz,
                              SampleType{0.707f}, sample_rate);
        hp2_.set_coefficients(BiquadT<SampleType>::Type::highpass, hz,
                              SampleType{0.707f}, sample_rate);
    }

    struct BandSplit { SampleType low, high; };

    BandSplit process(SampleType input) {
        SampleType low = lp2_.process(lp1_.process(input));
        SampleType high = hp2_.process(hp1_.process(input));
        return {low, high};
    }

    void reset() {
        lp1_.reset(); lp2_.reset();
        hp1_.reset(); hp2_.reset();
    }

private:
    BiquadT<SampleType> lp1_, lp2_, hp1_, hp2_;
};

using LinkwitzRiley = LinkwitzRileyT<float>;
using LinkwitzRiley64 = LinkwitzRileyT<double>;

} // namespace pulp::signal
