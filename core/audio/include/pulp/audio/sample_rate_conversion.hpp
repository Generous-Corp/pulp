#pragma once

#include <pulp/signal/sinc_resampler.hpp>

#include <span>

namespace pulp::audio {

/// Immutable, allocation-free read kernel after control-thread construction.
///
/// `cutoff` is normalized to the source Nyquist. Construction allocates the
/// phase table and is not audio-thread safe; read() is bounded and RT-safe.
class PreparedSampleRateConversion {
  public:
    explicit PreparedSampleRateConversion(double cutoff) {
        kernel_.build(32, 512, 10.0, cutoff);
    }

    float read(std::span<const float> source, double position) const noexcept {
        return kernel_.read(source.data(), static_cast<int>(source.size()), position);
    }

  private:
    signal::SincResampler kernel_;
};

} // namespace pulp::audio
