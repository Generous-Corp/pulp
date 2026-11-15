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

    static SpectralResult<std::shared_ptr<const SpectralAssetMorphConfig>>
    make(std::span<const std::uint8_t> bytes_a, std::span<const std::uint8_t> bytes_b,
         std::uint64_t length, std::size_t budget) {
        // The finite-stream rule bounds every grid/cutoff value before it is
        // used as signed runtime chronology. The codec caps count too.
        // Only profile 1, FFT 256, hop 64, mono is admitted; the endpoint
        // storage in SpectralAssetMorphState is sized for exactly that.
        const auto rule = SpectralFiniteStreamRule::make(256, 64, 1, length);
        if (rule.status == SpectralFiniteStreamRule::Status::capacity_exceeded)
            return {std::nullopt, SpectralError::CapacityExceeded};
        if (!rule)
            return {std::nullopt, SpectralError::InvalidFrame};
        const auto frames = rule.frame_count;
        auto first = signal::decode_spectral_stream(bytes_a, budget);
        if (!first)
            return {std::nullopt, SpectralError::InvalidFrame};
        if (first.memory.retained_bytes >= budget)
            return {std::nullopt, SpectralError::CapacityExceeded};
        auto second = signal::decode_spectral_stream(bytes_b, budget - first.memory.retained_bytes);
        if (!second)
            return {std::nullopt, SpectralError::InvalidFrame};
        const auto& ga = first.value->geometry;
        const auto& gb = second.value->geometry;
        if (ga.profile != 1 || ga.fft_size != rule.fft_size || ga.hop != rule.hop ||
            ga.channels != rule.channels || gb.profile != ga.profile ||
            gb.fft_size != ga.fft_size || gb.hop != ga.hop || gb.channels != ga.channels ||
            std::bit_cast<std::uint64_t>(gb.sample_rate) !=
                std::bit_cast<std::uint64_t>(ga.sample_rate) ||
            first.value->frame_count != frames || second.value->frame_count != frames)
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
    const SpectralAssetMorphConfig* assets = nullptr;
    signal::SpectralMorph morph;
    // Sized for the single admitted geometry; make() refuses every other one.
    std::array<std::complex<float>, 129> endpoint_a{}, endpoint_b{};
    double sum = 0;
    unsigned samples = 0;

    void reset() noexcept {
        sum = 0;
        samples = 0;
        morph.reset();
    }
    bool frame(std::uint64_t sequence, float amount,
               std::span<std::complex<float>> output) noexcept {
        if (sequence >= assets->a.frame_count) {
            std::fill(output.begin(), output.end(), std::complex<float>{});
            return true;
        }
        const auto& rule = assets->rule;
        for (std::size_t k = 0; k < endpoint_a.size(); ++k) {
            const auto p = rule.component(sequence, 0, k);
            endpoint_a[k] = {assets->a.components[p], assets->a.components[p + 1]};
            endpoint_b[k] = {assets->b.components[p], assets->b.components[p + 1]};
            // Refuse before the DSP's forgiving fallback can conceal bad data
            // or saturate a finite-component magnitude that overflows float.
            if (!std::isfinite(std::abs(endpoint_a[k])) || !std::isfinite(std::abs(endpoint_b[k])))
                return false;
        }
        const std::complex<float>* a[] = {endpoint_a.data()};
        const std::complex<float>* b[] = {endpoint_b.data()};
        std::complex<float>* out[] = {output.data()};
        return morph.process(a, b, out, 1, static_cast<int>(endpoint_a.size()), amount, amount);
    }
};
} // namespace pulp::host
