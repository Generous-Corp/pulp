#pragma once

/// @file oversampling_fir.hpp
/// Linear-phase 2x FIR stage used by the realtime oversampler.

#include <pulp/signal/fir_filter.hpp>
#include <pulp/signal/windowed_sinc_design.hpp>

#include <cstddef>
#include <vector>

namespace pulp::signal::detail {

template <typename SampleType> class LinearPhaseOversamplingStage2x {
  public:
    void configure(double passband_edge_fraction, double stopband_db, std::size_t taps) {
        // Fractions are expressed against the input rate. This filter runs at
        // twice that rate, so base Nyquist is 0.25 cycles/output-sample.
        const double passband = 0.5 * passband_edge_fraction;
        constexpr double stopband = 0.25;
        const double transition = stopband - passband;
        const double cutoff = passband + 0.5 * transition;
        const double beta = kaiser_beta_for_stopband(stopband_db);
        auto prototype = design_windowed_sinc(taps, cutoff, beta);

        std::vector<SampleType> up_even;
        std::vector<SampleType> up_odd;
        std::vector<SampleType> down_even;
        std::vector<SampleType> down_odd;
        up_even.reserve((prototype.size() + 1) / 2);
        up_odd.reserve(prototype.size() / 2);
        down_even.reserve((prototype.size() + 1) / 2);
        down_odd.reserve(prototype.size() / 2);
        for (std::size_t i = 0; i < prototype.size(); ++i) {
            const auto coefficient = static_cast<SampleType>(prototype[i]);
            auto& up_phase = (i & 1u) == 0u ? up_even : up_odd;
            auto& down_phase = (i & 1u) == 0u ? down_even : down_odd;
            up_phase.push_back(SampleType{2} * coefficient);
            down_phase.push_back(coefficient);
        }
        up_even_.set_coefficients(std::move(up_even));
        up_odd_.set_coefficients(std::move(up_odd));
        down_even_.set_coefficients(std::move(down_even));
        down_odd_.set_coefficients(std::move(down_odd));
        taps_ = taps;
    }

    void reset() {
        up_even_.reset();
        up_odd_.reset();
        down_even_.reset();
        down_odd_.reset();
        previous_odd_ = SampleType{0};
    }

    void upsample(SampleType input, SampleType& even, SampleType& odd) {
        even = up_even_.process(input);
        odd = up_odd_.process(input);
    }

    SampleType downsample(SampleType even, SampleType odd) {
        const SampleType output = down_even_.process(even) + down_odd_.process(previous_odd_);
        previous_odd_ = odd;
        return output;
    }

    std::size_t taps() const noexcept {
        return taps_;
    }

  private:
    FirFilterT<SampleType> up_even_;
    FirFilterT<SampleType> up_odd_;
    FirFilterT<SampleType> down_even_;
    FirFilterT<SampleType> down_odd_;
    SampleType previous_odd_ = SampleType{0};
    std::size_t taps_ = 0;
};

} // namespace pulp::signal::detail
