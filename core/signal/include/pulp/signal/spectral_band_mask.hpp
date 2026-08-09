#pragma once

/// @file spectral_band_mask.hpp
/// Fixed-capacity control-side compilation and frame-side application of a
/// zoomable spectral band layout.
///
/// `SpectralBandLayoutT` is a DSP contract, not a UI model. It keeps a stable
/// maximum of 64 slots, represents mute categorically, and maps the active
/// slots onto either a linear or logarithmic frequency viewport. The builder
/// materializes one immutable positive-frequency table for a prepared FFT.
/// Applying that table to a coherent channel group is allocation-free and
/// preserves every bin's phase and inter-channel relationship.

#include <pulp/signal/spectral_frame_engine.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pulp::signal {

inline constexpr std::size_t kSpectralBandMaskMaximumBands = 64;
inline constexpr std::size_t kSpectralBandMaskMaximumBins =
    static_cast<std::size_t>(kSpectralFrameEngineMaximumFftSize / 2 + 1);
inline constexpr std::uint32_t kSpectralBandMaskMaximumTransitionFrames = 4096;

enum class SpectralBandSpacing : std::uint8_t {
    linear,
    logarithmic,
};

enum class SpectralBandEdgePolicy : std::uint8_t {
    mute_outside,
    extend_edge_band,
};

enum class SpectralMaskBoundaryKernel : std::uint8_t {
    hard,
    raised_cosine,
};

template <typename SampleType>
struct SpectralBandT {
    static_assert(std::is_floating_point_v<SampleType>);
    SampleType gain_db = SampleType{0};
    bool muted = false;
};

template <typename SampleType>
struct SpectralBandLayoutT {
    static_assert(std::is_floating_point_v<SampleType>);

    std::uint32_t active_bands = 32;
    SampleType min_hz = SampleType{20};
    SampleType max_hz = SampleType{20000};
    SpectralBandSpacing spacing = SpectralBandSpacing::logarithmic;
    SpectralBandEdgePolicy edge_policy = SpectralBandEdgePolicy::mute_outside;
    SpectralMaskBoundaryKernel boundary_kernel =
        SpectralMaskBoundaryKernel::raised_cosine;

    /// Half-width of each boundary transition as a fraction of one band in
    /// the selected coordinate system. 0 is a hard step; 0.5 lets adjacent
    /// transitions meet at band centers without overlapping.
    SampleType transition_fraction = SampleType{0.08};

    /// Requested frame-domain transition duration. The static builder stores
    /// this metadata; a streaming processor owns interpolation over time.
    std::uint32_t transition_frames = 4;
    std::uint64_t version = 0;
    std::array<SpectralBandT<SampleType>, kSpectralBandMaskMaximumBands> bands{};
};

template <typename SampleType>
struct SpectralMaskTableT {
    static_assert(std::is_floating_point_v<SampleType>);

    int fft_size = 0;
    int num_bins = 0;
    SampleType sample_rate = SampleType{0};
    SampleType effective_min_hz = SampleType{0};
    SampleType effective_max_hz = SampleType{0};
    std::uint32_t active_bands = 0;
    std::uint32_t transition_frames = 0;
    std::uint64_t version = 0;
    std::array<SampleType, kSpectralBandMaskMaximumBands + 1> band_edges_hz{};
    std::array<SampleType, kSpectralBandMaskMaximumBins> gain_linear{};
};

namespace detail {

template <typename SampleType>
inline bool valid_spectral_band_layout(const SpectralBandLayoutT<SampleType>& layout,
                                       SampleType nyquist,
                                       SampleType& effective_min,
                                       SampleType& effective_max) noexcept {
    const bool valid_spacing = layout.spacing == SpectralBandSpacing::linear
                            || layout.spacing == SpectralBandSpacing::logarithmic;
    const bool valid_edge_policy =
        layout.edge_policy == SpectralBandEdgePolicy::mute_outside
        || layout.edge_policy == SpectralBandEdgePolicy::extend_edge_band;
    const bool valid_kernel = layout.boundary_kernel == SpectralMaskBoundaryKernel::hard
                           || layout.boundary_kernel
                                  == SpectralMaskBoundaryKernel::raised_cosine;
    if (layout.active_bands == 0
        || layout.active_bands > kSpectralBandMaskMaximumBands
        || !valid_spacing
        || !valid_edge_policy
        || !valid_kernel
        || !std::isfinite(layout.min_hz)
        || !std::isfinite(layout.max_hz)
        || !std::isfinite(layout.transition_fraction)
        || layout.min_hz < SampleType{0}
        || layout.max_hz <= layout.min_hz
        || layout.transition_fraction < SampleType{0}
        || layout.transition_fraction > SampleType{0.5}
        || layout.transition_frames > kSpectralBandMaskMaximumTransitionFrames)
        return false;

    effective_min = std::clamp(layout.min_hz, SampleType{0}, nyquist);
    effective_max = std::clamp(layout.max_hz, SampleType{0}, nyquist);
    if (!(effective_max > effective_min)) return false;
    if (layout.spacing == SpectralBandSpacing::logarithmic
        && !(effective_min > SampleType{0}))
        return false;

    for (std::uint32_t band = 0; band < layout.active_bands; ++band) {
        const auto gain_db = layout.bands[band].gain_db;
        // A broad but bounded framework range. Products may expose a narrower
        // host contract (Spectr uses -24..+24 dB).
        if (!std::isfinite(gain_db)
            || gain_db < SampleType{-192}
            || gain_db > SampleType{48})
            return false;
    }
    return true;
}

template <typename SampleType>
inline SampleType band_coordinate(SampleType hz,
                                  SampleType min_hz,
                                  SampleType max_hz,
                                  SpectralBandSpacing spacing) noexcept {
    if (spacing == SpectralBandSpacing::linear)
        return (hz - min_hz) / (max_hz - min_hz);
    return std::log(hz / min_hz) / std::log(max_hz / min_hz);
}

template <typename SampleType>
inline SampleType frequency_at_coordinate(SampleType coordinate,
                                          SampleType min_hz,
                                          SampleType max_hz,
                                          SpectralBandSpacing spacing) noexcept {
    if (spacing == SpectralBandSpacing::linear)
        return min_hz + coordinate * (max_hz - min_hz);
    return min_hz * std::pow(max_hz / min_hz, coordinate);
}

template <typename SampleType>
inline SampleType raised_cosine(SampleType position) noexcept {
    const auto x = std::clamp(position, SampleType{0}, SampleType{1});
    constexpr SampleType pi =
        static_cast<SampleType>(3.14159265358979323846264338327950288L);
    return SampleType{0.5} - SampleType{0.5} * std::cos(pi * x);
}

template <typename SampleType>
inline SampleType band_gain(const SpectralBandLayoutT<SampleType>& layout,
                            std::uint32_t index) noexcept {
    const auto& band = layout.bands[index];
    return band.muted
        ? SampleType{0}
        : std::pow(SampleType{10}, band.gain_db * SampleType{0.05});
}

} // namespace detail

/// Compile a complete positive-frequency gain table. The operation is
/// failure-atomic: `out_table` is unchanged if geometry or controls are bad.
/// This is a control-thread operation; it performs no dynamic allocation.
template <typename SampleType>
bool build_spectral_mask(const SpectralBandLayoutT<SampleType>& layout,
                         int fft_size,
                         SampleType sample_rate,
                         SpectralMaskTableT<SampleType>& out_table) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    if (!is_valid_spectral_frame_geometry(fft_size, fft_size / 4)
        || !std::isfinite(sample_rate)
        || sample_rate <= SampleType{0})
        return false;

    const int num_bins = fft_size / 2 + 1;
    if (num_bins <= 1
        || static_cast<std::size_t>(num_bins) > kSpectralBandMaskMaximumBins)
        return false;

    const SampleType nyquist = sample_rate * SampleType{0.5};
    SampleType min_hz = SampleType{0};
    SampleType max_hz = SampleType{0};
    if (!detail::valid_spectral_band_layout(layout, nyquist, min_hz, max_hz))
        return false;

    SpectralMaskTableT<SampleType> candidate{};
    candidate.fft_size = fft_size;
    candidate.num_bins = num_bins;
    candidate.sample_rate = sample_rate;
    candidate.effective_min_hz = min_hz;
    candidate.effective_max_hz = max_hz;
    candidate.active_bands = layout.active_bands;
    candidate.transition_frames = layout.transition_frames;
    candidate.version = layout.version;

    const auto band_count = layout.active_bands;
    for (std::uint32_t edge = 0; edge <= band_count; ++edge) {
        const auto coordinate = static_cast<SampleType>(edge)
                              / static_cast<SampleType>(band_count);
        candidate.band_edges_hz[edge] = detail::frequency_at_coordinate(
            coordinate, min_hz, max_hz, layout.spacing);
    }

    const SampleType half_width = layout.transition_fraction
                                / static_cast<SampleType>(band_count);
    const bool smooth = layout.boundary_kernel
                            == SpectralMaskBoundaryKernel::raised_cosine
                     && half_width > SampleType{0};

    for (int bin = 0; bin < num_bins; ++bin) {
        const SampleType hz = (static_cast<SampleType>(bin)
                             / static_cast<SampleType>(fft_size)) * sample_rate;
        SampleType gain = SampleType{0};

        if (hz < min_hz) {
            gain = layout.edge_policy == SpectralBandEdgePolicy::extend_edge_band
                ? detail::band_gain(layout, 0)
                : SampleType{0};
        } else if (hz > max_hz) {
            gain = layout.edge_policy == SpectralBandEdgePolicy::extend_edge_band
                ? detail::band_gain(layout, band_count - 1)
                : SampleType{0};
        } else {
            const SampleType coordinate = std::clamp(
                detail::band_coordinate(hz, min_hz, max_hz, layout.spacing),
                SampleType{0}, SampleType{1});
            const SampleType scaled = coordinate * static_cast<SampleType>(band_count);
            const auto band = std::min<std::uint32_t>(
                static_cast<std::uint32_t>(scaled), band_count - 1);
            gain = detail::band_gain(layout, band);

            if (smooth) {
                // Internal boundary nearest this bin.
                const auto boundary_index = static_cast<std::uint32_t>(
                    std::floor(scaled + SampleType{0.5}));
                if (boundary_index > 0 && boundary_index < band_count) {
                    const SampleType boundary = static_cast<SampleType>(boundary_index)
                                              / static_cast<SampleType>(band_count);
                    if (std::abs(coordinate - boundary) <= half_width) {
                        const auto left = detail::band_gain(layout, boundary_index - 1);
                        const auto right = detail::band_gain(layout, boundary_index);
                        const auto position = (coordinate - (boundary - half_width))
                                            / (SampleType{2} * half_width);
                        const auto mix = detail::raised_cosine(position);
                        gain = left + (right - left) * mix;
                    }
                } else if (layout.edge_policy == SpectralBandEdgePolicy::mute_outside) {
                    if (coordinate <= half_width) {
                        const auto mix = detail::raised_cosine(
                            (coordinate + half_width) / (SampleType{2} * half_width));
                        gain = detail::band_gain(layout, 0) * mix;
                    } else if (coordinate >= SampleType{1} - half_width) {
                        const auto mix = detail::raised_cosine(
                            (coordinate - (SampleType{1} - half_width))
                            / (SampleType{2} * half_width));
                        gain = detail::band_gain(layout, band_count - 1)
                             * (SampleType{1} - mix);
                    }
                }
            }
        }
        candidate.gain_linear[static_cast<std::size_t>(bin)] = gain;
    }

    out_table = candidate;
    return true;
}

/// Multiply one coherent positive-frequency frame group by a prepared mask.
/// Geometry and table values are validated before mutation, so failure is
/// atomic. No allocation, locks, or I/O occur.
template <typename SampleType>
bool apply_spectral_mask(std::complex<SampleType>* const* frames,
                         int channels,
                         int num_bins,
                         const SpectralMaskTableT<SampleType>& table) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    if (frames == nullptr || channels <= 0 || num_bins <= 0
        || table.num_bins != num_bins
        || table.fft_size < kSpectralFrameEngineMinimumFftSize
        || table.fft_size > kSpectralFrameEngineMaximumFftSize
        || table.fft_size / 2 + 1 != num_bins
        || !std::isfinite(table.sample_rate)
        || table.sample_rate <= SampleType{0}
        || static_cast<std::size_t>(num_bins) > kSpectralBandMaskMaximumBins)
        return false;
    for (int channel = 0; channel < channels; ++channel)
        if (frames[channel] == nullptr) return false;
    for (int bin = 0; bin < num_bins; ++bin) {
        const auto gain = table.gain_linear[static_cast<std::size_t>(bin)];
        if (!std::isfinite(gain) || gain < SampleType{0}) return false;
    }

    for (int channel = 0; channel < channels; ++channel) {
        for (int bin = 0; bin < num_bins; ++bin) {
            auto& value = frames[channel][bin];
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
                value = {};
            else {
                const auto gain = table.gain_linear[static_cast<std::size_t>(bin)];
                if (gain == SampleType{0}) {
                    value = {};
                    continue;
                }
                const auto maximum = std::numeric_limits<SampleType>::max();
                const auto limit = maximum / gain;
                const auto scale_component = [gain, limit, maximum](SampleType component) {
                    return std::abs(component) > limit
                        ? std::copysign(maximum, component)
                        : component * gain;
                };
                value = {scale_component(value.real()), scale_component(value.imag())};
            }
        }
    }
    return true;
}

using SpectralBand = SpectralBandT<float>;
using SpectralBand64 = SpectralBandT<double>;
using SpectralBandLayout = SpectralBandLayoutT<float>;
using SpectralBandLayout64 = SpectralBandLayoutT<double>;
using SpectralMaskTable = SpectralMaskTableT<float>;
using SpectralMaskTable64 = SpectralMaskTableT<double>;

} // namespace pulp::signal
