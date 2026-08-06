#pragma once

/// @file velvet_noise.hpp
/// Coordinate-keyed velvet-noise grid draws.

#include <pulp/signal/rng.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

template <typename SampleType> struct VelvetNoiseDrawT {
    SampleType jitter{};
    int sign = 1;

    SampleType value() const noexcept {
        return static_cast<SampleType>(sign);
    }
};

/// Pure draw for one velvet grid cell. Coordinates, rather than call order,
/// select the draw, so a full build and an incremental build are identical.
template <typename SampleType = double>
inline VelvetNoiseDrawT<SampleType> velvet_noise_draw(std::uint64_t seed,
                                                      std::uint64_t index) noexcept {
    return {
        unit_from<SampleType>(mix64(seed, index, 0u)),
        (mix64(seed, index, 1u) & 1ull) ? 1 : -1,
    };
}

/// Converts a grid draw to its zero-based sample offset. A cell of width one
/// always lands at zero; wider cells use the same nearest-sample rule as Pulp's
/// existing ambience tap builder.
template <typename SampleType>
inline int velvet_noise_offset(const VelvetNoiseDrawT<SampleType>& draw,
                               SampleType grid_samples) noexcept {
    if (!(std::isfinite(grid_samples) && grid_samples > SampleType{1}))
        return 0;
    const auto width = std::max(SampleType{}, grid_samples - SampleType{1});
    return std::max(0, static_cast<int>(std::lround(draw.jitter * width)));
}

/// Sequential convenience wrapper over the coordinate-keyed pure draw.
///
/// RT contract: fixed-size integer state only; no allocation, locks, clocks,
/// or hidden global stream. Resetting restores index zero exactly.
template <typename SampleType = float> class VelvetNoiseGridT {
  public:
    void set_seed(std::uint64_t seed) noexcept {
        seed_ = seed;
    }
    void reset() noexcept {
        index_ = 0;
    }
    VelvetNoiseDrawT<SampleType> next() noexcept {
        return velvet_noise_draw<SampleType>(seed_, index_++);
    }
    std::uint64_t index() const noexcept {
        return index_;
    }
    std::uint64_t seed() const noexcept {
        return seed_;
    }

  private:
    std::uint64_t seed_ = kGoldenGamma;
    std::uint64_t index_ = 0;
};

using VelvetNoiseGrid = VelvetNoiseGridT<float>;
using VelvetNoiseGrid64 = VelvetNoiseGridT<double>;

} // namespace pulp::signal
