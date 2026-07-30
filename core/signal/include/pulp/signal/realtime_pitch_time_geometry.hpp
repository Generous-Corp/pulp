#pragma once

/// @file realtime_pitch_time_geometry.hpp
/// Pure configuration and retained-storage admission for the realtime
/// pitch/time processor. Each owned DSP primitive accounts for its own backing
/// stores; this layer only composes those contracts with the stream buffers.

#include <pulp/signal/freeze_hold.hpp>
#include <pulp/signal/multichannel_phase_coordinator.hpp>
#include <pulp/signal/noise_morpher.hpp>
#include <pulp/signal/sinc_resampler.hpp>
#include <pulp/signal/spectral_envelope_shifter.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>
#include <pulp/signal/stn_decomposer.hpp>
#include <pulp/signal/transient_phase_policy.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>

namespace pulp::signal {

enum class PitchTimeQuality { quality, low_latency };
enum class PitchTimeMode { realtime_pitch, time_stretch };
enum class FormantMode { follow, preserve };

enum class PitchTimeStreamFeedStatus { accepted, backpressure, input_closed, invalid_request };
enum class PitchTimeStreamFinalizeStatus {
    draining,
    backpressure,
    complete,
    invalid_mode,
    invalid_request,
};
enum class PitchTimeStreamFinalizePlanStatus {
    ready,
    needs_drain,
    complete,
    invalid_mode,
    invalid_request,
};

struct PitchTimeStreamFinalizePlan {
    PitchTimeStreamFinalizePlanStatus status = PitchTimeStreamFinalizePlanStatus::invalid_mode;
    int samples = 0;
};

enum class PitchTimePrepareStatus {
    prepared,
    invalid_sample_rate,
    invalid_channel_count,
    invalid_max_block,
    invalid_max_time_ratio,
    invalid_max_pitch_semitones,
    invalid_spectral_geometry,
    unrepresentable_capacity,
};

struct RealtimePitchTimeConfig {
    PitchTimeMode mode = PitchTimeMode::realtime_pitch;
    PitchTimeQuality quality = PitchTimeQuality::quality;
    int channels = 1;
    int max_block = 4096;
    float max_pitch_semitones = 12.0f;
    float max_time_ratio = 2.0f;
    float pitch_smoothing_seconds = 0.03f;
    FormantMode formant_mode = FormantMode::follow;
    int true_envelope_iterations = 3;
    bool transient_preservation = true;
    float transient_sensitivity = 0.0f;
    bool noise_morphing = false;
    bool sinc_resampling = false;
    int fft_size = 0;
    int analysis_hop = 0;
};

inline constexpr int kRealtimePitchTimeMaximumChannels = 64;

template <typename SampleType>
struct RealtimePitchTimePreparedGeometry {
    SpectralFrameEngineConfig engine_config;
    int fft_size = 0;
    int analysis_hop = 0;
    int ring_size = 0;
    std::uint64_t stretch_ring_elements = 0;
    std::uint64_t drain_elements = 0;
    std::uint64_t finalize_zero_elements = 0;
    std::uint64_t retained_bytes = 0;
};

/// Derive all processor geometry and compose the retained-storage contracts of
/// its owned DSP primitives without changing processor state.
template <typename SampleType>
PitchTimePrepareStatus checked_realtime_pitch_time_prepared_geometry(
    const RealtimePitchTimeConfig& config, double max_pitch_ratio,
    std::uint64_t requested_max_bytes,
    RealtimePitchTimePreparedGeometry<SampleType>& prepared) noexcept {
    const auto target_max_bytes = std::min(requested_max_bytes, kTargetAddressMaximumBytes);
    const bool quality = config.quality == PitchTimeQuality::quality;
    int fft_size = quality ? 4096 : 1024;
    int analysis_hop = quality ? 512 : 256;
    if (config.fft_size != 0 || config.analysis_hop != 0) {
        if (!is_valid_spectral_frame_geometry(config.fft_size, config.analysis_hop)
            || (config.fft_size % config.analysis_hop) != 0)
            return PitchTimePrepareStatus::invalid_spectral_geometry;
        fft_size = config.fft_size;
        analysis_hop = config.analysis_hop;
    }

    const double max_stretch = config.mode == PitchTimeMode::time_stretch
        ? static_cast<double>(config.max_time_ratio)
        : max_pitch_ratio;
    const double synthesis_hop =
        std::ceil(max_stretch * static_cast<double>(analysis_hop)) + 1.0;
    if (!std::isfinite(synthesis_hop)
        || synthesis_hop > static_cast<double>(std::numeric_limits<int>::max()))
        return PitchTimePrepareStatus::unrepresentable_capacity;

    RealtimePitchTimePreparedGeometry<SampleType> candidate;
    candidate.fft_size = fft_size;
    candidate.analysis_hop = analysis_hop;
    candidate.engine_config = {fft_size, analysis_hop, config.channels,
                               std::max(config.max_block, analysis_hop),
                               static_cast<int>(synthesis_hop)};
    const auto engine_geometry = checked_spectral_frame_engine_geometry<SampleType>(
        candidate.engine_config, target_max_bytes);
    if (!engine_geometry) return PitchTimePrepareStatus::unrepresentable_capacity;

    const std::int64_t span_base = static_cast<std::int64_t>(fft_size)
                                 + 2 * static_cast<std::int64_t>(analysis_hop)
                                 + candidate.engine_config.max_block;
    const long double span = static_cast<long double>(span_base)
                                 * static_cast<long double>(std::max(max_stretch, 1.0))
                             + static_cast<long double>(fft_size) + 64.0L;
    if (!std::isfinite(span)
        || span > static_cast<long double>(kSpectralFrameEngineMaximumRingSize))
        return PitchTimePrepareStatus::unrepresentable_capacity;
    std::uint64_t ring_size = 1;
    const auto required_ring_size = static_cast<std::uint64_t>(std::ceil(span));
    while (ring_size < required_ring_size) ring_size <<= 1u;
    candidate.ring_size = static_cast<int>(ring_size);

    const auto channels = static_cast<std::uint64_t>(config.channels);
    const auto bins = static_cast<std::uint64_t>(fft_size / 2 + 1);
    std::uint64_t channel_hop_elements = 0;
    if (!checked_capacity_product(channels, ring_size, UINT64_MAX,
                                  candidate.stretch_ring_elements)
        || !checked_capacity_product(
            channels, static_cast<std::uint64_t>(candidate.engine_config.max_synthesis_hop),
            UINT64_MAX, channel_hop_elements)
        || !checked_capacity_product(channel_hop_elements, 4u, UINT64_MAX,
                                     candidate.drain_elements)
        || !checked_capacity_product(
            channels, static_cast<std::uint64_t>(candidate.engine_config.max_block), UINT64_MAX,
            candidate.finalize_zero_elements))
        return PitchTimePrepareStatus::unrepresentable_capacity;

    CheckedRetainedByteCharge charge(target_max_bytes);
    if (!charge.add_retained_bytes(engine_geometry->retained_bytes)
        || !charge.add<SampleType>(candidate.stretch_ring_elements)
        || !charge.add<SampleType>(candidate.drain_elements)
        || !charge.add<SampleType>(candidate.finalize_zero_elements)
        || !charge.add<SampleType*>(channels) || !charge.add<const SampleType*>(channels))
        return PitchTimePrepareStatus::unrepresentable_capacity;

    std::uint64_t component_bytes = 0;
    const auto component_fits = [&charge, &component_bytes](bool valid) noexcept {
        return valid && charge.add_retained_bytes(component_bytes);
    };
    typename FreezeHoldT<SampleType>::Config freeze_config;
    freeze_config.fft_size = fft_size;
    freeze_config.channels = config.channels;
    freeze_config.analysis_hop = analysis_hop;
    if (!component_fits(
            MultichannelPhaseCoordinatorT<SampleType>::checked_retained_bytes(
                fft_size, target_max_bytes, component_bytes))
        || !component_fits(
            TransientPhasePolicyT<SampleType>::checked_retained_bytes(
                fft_size, target_max_bytes, component_bytes))
        || !component_fits(
            FreezeHoldT<SampleType>::checked_retained_bytes(
                freeze_config, target_max_bytes, component_bytes))
        || !component_fits(
            SpectralEnvelopeShifterT<SampleType>::checked_retained_bytes(
                fft_size, target_max_bytes, component_bytes)))
        return PitchTimePrepareStatus::unrepresentable_capacity;

    if (config.noise_morphing) {
        StnConfig stn_config;
        stn_config.num_bins = static_cast<int>(bins);
        stn_config.time_median = quality ? 7 : 5;
        stn_config.freq_median = quality ? 11 : 7;
        if (!component_fits(
                StnDecomposerT<SampleType>::checked_retained_bytes(
                    stn_config, target_max_bytes, component_bytes)))
            return PitchTimePrepareStatus::unrepresentable_capacity;
        std::uint64_t morpher_bytes = 0;
        std::uint64_t all_morpher_bytes = 0;
        if (!NoiseMorpherT<SampleType>::checked_retained_bytes(
                static_cast<int>(bins), target_max_bytes, morpher_bytes)
            || !checked_capacity_product(morpher_bytes, channels, UINT64_MAX,
                                         all_morpher_bytes)
            || !charge.add<NoiseMorpherT<SampleType>>(channels)
            || !charge.add_retained_bytes(all_morpher_bytes)
            || !charge.add<SampleType>(bins) || !charge.add<SampleType>(channels * bins)
            || !charge.add<std::complex<SampleType>>(bins))
            return PitchTimePrepareStatus::unrepresentable_capacity;
    }
    if (config.sinc_resampling) {
        if (!component_fits(
                SincResamplerT<SampleType>::checked_retained_bytes(
                    16, 512, target_max_bytes, component_bytes))
            || !charge.add<SampleType>(32u))
            return PitchTimePrepareStatus::unrepresentable_capacity;
    }

    candidate.retained_bytes = charge.total();
    prepared = candidate;
    return PitchTimePrepareStatus::prepared;
}

} // namespace pulp::signal
