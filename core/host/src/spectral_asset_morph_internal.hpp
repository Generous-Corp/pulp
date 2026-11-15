#pragma once

#include <pulp/host/spectral_graph.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/signal/spectral_frame_codec.hpp>
#include <pulp/signal/spectral_morph.hpp>

#include "spectral_finite_stream_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace pulp::host {

// Constructed on the stopped control thread and retained by prepared nodes.
// The callback borrows this immutable state without copying ownership handles.
class SpectralAssetMorphConfig {
  public:
    signal::OwnedSpectralStream a, b;
    std::array<std::uint8_t, 32> hash_a{}, hash_b{};
    SpectralFiniteStreamRule rule;
    std::uint64_t source_samples = 0, render_frames = 0;

    std::size_t retained_bytes() const noexcept {
        return sizeof(*this) +
               (a.frame_count + b.frame_count) * sizeof(signal::SpectralFrameStamp) +
               (a.component_count + b.component_count) * sizeof(float);
    }
    // Per-node endpoint scratch a prepared node allocates for this geometry.
    std::size_t endpoint_bytes() const noexcept {
        return 2 * rule.channels * rule.bins * sizeof(std::complex<float>);
    }

    // `refusal`, when given, receives the rule status that refused the call
    // (unset when the refusal came from decoding or identity matching).
    static SpectralResult<std::shared_ptr<const SpectralAssetMorphConfig>>
    make(std::span<const std::uint8_t> bytes_a, std::span<const std::uint8_t> bytes_b,
         std::uint64_t length, std::size_t budget,
         SpectralFiniteStreamRule::Status* refusal = nullptr) {
        using Status = SpectralFiniteStreamRule::Status;
        if (refusal)
            *refusal = Status::unset;
        // A codec resource limit is a capacity refusal, not a malformed asset.
        const auto decode_error = [](signal::SpectralCodecError e) {
            return e == signal::SpectralCodecError::ResourceLimit ? SpectralError::CapacityExceeded
                                                                  : SpectralError::InvalidFrame;
        };
        auto first = signal::decode_spectral_stream(bytes_a, budget);
        if (!first)
            return {std::nullopt, decode_error(first.error)};
        if (first.memory.retained_bytes >= budget)
            return {std::nullopt, SpectralError::CapacityExceeded};
        const auto& ga = first.value->geometry;
        // The geometry comes from the decoded stream; the finite-stream rule
        // admits a subset of what the codec decodes and bounds every grid and
        // cutoff value before it is used as signed runtime chronology.
        const auto rule = SpectralFiniteStreamRule::make(ga.fft_size, ga.hop, ga.channels, length);
        if (!rule && refusal)
            *refusal = rule.status;
        if (rule.geometry_refused())
            return {std::nullopt, SpectralError::UnsupportedOperation};
        if (rule.status == Status::capacity_exceeded)
            return {std::nullopt, SpectralError::CapacityExceeded};
        if (!rule)
            return {std::nullopt, SpectralError::InvalidFrame};
        const auto frames = rule.frame_count;
        if (ga.profile != 1 || first.value->frame_count != frames)
            return {std::nullopt, SpectralError::IdentityMismatch};
        auto second = signal::decode_spectral_stream(bytes_b, budget - first.memory.retained_bytes);
        if (!second)
            return {std::nullopt, decode_error(second.error)};
        const auto& gb = second.value->geometry;
        if (gb.profile != ga.profile || gb.fft_size != ga.fft_size || gb.hop != ga.hop ||
            gb.channels != ga.channels ||
            std::bit_cast<std::uint64_t>(gb.sample_rate) !=
                std::bit_cast<std::uint64_t>(ga.sample_rate) ||
            second.value->frame_count != frames)
            return {std::nullopt, SpectralError::IdentityMismatch};
        auto config = std::make_shared<SpectralAssetMorphConfig>();
        config->a = std::move(*first.value);
        config->b = std::move(*second.value);
        config->rule = rule;
        config->source_samples = length;
        config->render_frames = rule.render_cutoff;
        if (config->retained_bytes() > budget)
            return {std::nullopt, SpectralError::CapacityExceeded};
        const auto ha = runtime::sha256(bytes_a.data(), bytes_a.size());
        const auto hb = runtime::sha256(bytes_b.data(), bytes_b.size());
        std::copy(ha.begin(), ha.end(), config->hash_a.begin());
        std::copy(hb.begin(), hb.end(), config->hash_b.begin());
        return {std::move(config)};
    }
};

struct SpectralAssetMorphState {
    static constexpr std::size_t max_channels = 16;
    const SpectralAssetMorphConfig* assets = nullptr;
    signal::SpectralMorph morph;
    // Channel-major endpoint frames, sized by prepare() on the stopped control
    // thread from the rule; frame() only reads and writes the fixed storage.
    std::vector<std::complex<float>> endpoint_a, endpoint_b;
    double sum = 0;
    unsigned samples = 0;

    bool prepare() {
        if (!assets || assets->rule.channels < 1 || assets->rule.channels > max_channels)
            return false;
        const auto& rule = assets->rule;
        if (!morph.prepare(static_cast<int>(rule.channels), static_cast<int>(rule.bins)))
            return false;
        const auto size = static_cast<std::size_t>(rule.channels * rule.bins);
        endpoint_a.assign(size, {});
        endpoint_b.assign(size, {});
        return true;
    }
    void reset() noexcept {
        sum = 0;
        samples = 0;
        morph.reset();
    }
    bool frame(std::uint64_t sequence, float amount,
               std::span<std::complex<float>> output) noexcept {
        const auto& rule = assets->rule;
        const auto channels = static_cast<std::size_t>(rule.channels);
        const auto bins = static_cast<std::size_t>(rule.bins);
        if (output.size() != channels * bins || endpoint_a.size() != output.size() ||
            endpoint_b.size() != output.size())
            return false;
        if (sequence >= assets->a.frame_count) {
            std::fill(output.begin(), output.end(), std::complex<float>{});
            return true;
        }
        std::array<const std::complex<float>*, max_channels> a{}, b{};
        std::array<std::complex<float>*, max_channels> out{};
        for (std::size_t c = 0; c < channels; ++c) {
            for (std::size_t k = 0; k < bins; ++k) {
                const auto p = rule.component(sequence, c, k);
                auto& ea = endpoint_a[c * bins + k];
                auto& eb = endpoint_b[c * bins + k];
                ea = {assets->a.components[p], assets->a.components[p + 1]};
                eb = {assets->b.components[p], assets->b.components[p + 1]};
                // Refuse before the DSP's forgiving fallback can conceal bad
                // data or saturate a finite magnitude that overflows float.
                if (!std::isfinite(std::abs(ea)) || !std::isfinite(std::abs(eb)))
                    return false;
            }
            a[c] = endpoint_a.data() + c * bins;
            b[c] = endpoint_b.data() + c * bins;
            out[c] = output.data() + c * bins;
        }
        return morph.process(a.data(), b.data(), out.data(), static_cast<int>(channels),
                             static_cast<int>(bins), amount, amount);
    }
};
} // namespace pulp::host
