#pragma once

#include <pulp/signal/sinc_resampler.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
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
        if (source.empty() ||
            source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            !std::isfinite(position))
            return 0.0f;
        position = std::clamp(position, 0.0, static_cast<double>(source.size() - 1));
        return kernel_.read(source.data(), static_cast<int>(source.size()), position);
    }

  private:
    signal::SincResampler kernel_;
};

} // namespace pulp::audio
