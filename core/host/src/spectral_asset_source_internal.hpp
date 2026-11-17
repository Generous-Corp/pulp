#pragma once

#include <pulp/host/spectral_graph.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/signal/spectral_frame_codec.hpp>

#include "spectral_asset_stream_internal.hpp"

#include <algorithm>
#include <array>
#include <complex>
#include <cstdint>
#include <memory>
#include <span>

namespace pulp::host {

// One immutable decoded codec stream, constructed on the stopped control
// thread and retained by prepared nodes. The callback borrows it and copies a
// stored frame's components into the node's fixed working storage; it never
// changes a stored value.
class SpectralAssetSourceConfig {
  public:
    signal::OwnedSpectralStream stream;
    std::array<std::uint8_t, 32> hash{};
    SpectralFiniteStreamRule rule;

    std::size_t retained_bytes() const noexcept {
        return sizeof(*this) + stream.frame_count * sizeof(signal::SpectralFrameStamp) +
               stream.component_count * sizeof(float);
    }

    // Channel-major bins of frame `sequence` at stream position `origin`.
    // Drain frames past the stored stream are zero. Returns false, writing
    // nothing, when the output is the wrong size or the stored stamp does not
    // sit at the runtime origin.
    bool frame(std::uint64_t sequence, std::int64_t origin,
               std::span<std::complex<float>> output) const noexcept {
        if (output.size() != rule.channels * rule.bins)
            return false;
        if (sequence >= stream.frame_count) {
            std::fill(output.begin(), output.end(), std::complex<float>{});
            return true;
        }
        if (stream.frames[sequence].sequence != sequence ||
            stream.frames[sequence].origin != origin)
            return false;
        const auto* components = stream.components.get() + rule.component(sequence, 0, 0);
        for (std::size_t i = 0; i < output.size(); ++i)
            output[i] = {components[2 * i], components[2 * i + 1]};
        return true;
    }

    // `refusal`, when given, receives the rule status that refused the call
    // (unset when the refusal came from decoding or identity matching).
    static SpectralResult<std::shared_ptr<const SpectralAssetSourceConfig>>
    make(std::span<const std::uint8_t> bytes, std::uint64_t length, std::size_t budget,
         SpectralFiniteStreamRule::Status* refusal = nullptr) {
        auto decoded = decode_finite_spectral_stream(
            bytes, length, budget, SpectralFiniteStreamRule::current_policy, refusal);
        if (!decoded)
            return {std::nullopt, decoded.error};
        auto config = std::make_shared<SpectralAssetSourceConfig>();
        config->stream = std::move(decoded.stream);
        config->rule = decoded.rule;
        if (config->retained_bytes() > budget)
            return {std::nullopt, SpectralError::CapacityExceeded};
        const auto digest = runtime::sha256(bytes.data(), bytes.size());
        std::copy(digest.begin(), digest.end(), config->hash.begin());
        return {std::move(config)};
    }
};

} // namespace pulp::host
