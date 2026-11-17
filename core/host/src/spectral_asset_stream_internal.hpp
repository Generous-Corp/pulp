#pragma once

#include <pulp/host/spectral_graph.hpp>
#include <pulp/signal/spectral_frame_codec.hpp>

#include "spectral_finite_stream_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace pulp::host {

// A codec resource limit is a capacity refusal, not a malformed asset.
constexpr SpectralError spectral_codec_refusal(signal::SpectralCodecError e) noexcept {
    return e == signal::SpectralCodecError::ResourceLimit ? SpectralError::CapacityExceeded
                                                          : SpectralError::InvalidFrame;
}

// One decoded profile-1 stream admitted by the finite-stream rule at the
// stream's own geometry, the caller's source length and the given policy.
struct DecodedFiniteSpectralStream {
    signal::OwnedSpectralStream stream;
    SpectralFiniteStreamRule rule;
    std::uint64_t retained_bytes = 0;
    SpectralError error = SpectralError::None;

    explicit operator bool() const noexcept {
        return error == SpectralError::None;
    }
};

// Decodes one asset on the stopped control thread and admits it as a finite
// stream: the geometry comes from the decoded stream, the rule bounds every
// grid and cutoff value before it is used as signed runtime chronology, and
// the stored frame count must equal the rule's grid count for `length`.
// `refusal`, when given, receives the rule status that refused the call
// (unset when the refusal came from decoding or identity matching).
inline DecodedFiniteSpectralStream
decode_finite_spectral_stream(std::span<const std::uint8_t> bytes, std::uint64_t length,
                              std::size_t budget, SpectralFiniteStreamRule::Policy policy,
                              SpectralFiniteStreamRule::Status* refusal = nullptr) {
    using Status = SpectralFiniteStreamRule::Status;
    DecodedFiniteSpectralStream out;
    if (refusal)
        *refusal = Status::unset;
    auto decoded = signal::decode_spectral_stream(bytes, budget);
    if (!decoded) {
        out.error = spectral_codec_refusal(decoded.error);
        return out;
    }
    if (decoded.memory.retained_bytes >= budget) {
        out.error = SpectralError::CapacityExceeded;
        return out;
    }
    const auto& g = decoded.value->geometry;
    const auto rule = SpectralFiniteStreamRule::make(g.fft_size, g.hop, g.channels, length, policy);
    if (!rule && refusal)
        *refusal = rule.status;
    if (rule.geometry_refused())
        out.error = SpectralError::UnsupportedOperation;
    else if (rule.status == Status::capacity_exceeded)
        out.error = SpectralError::CapacityExceeded;
    else if (!rule)
        out.error = SpectralError::InvalidFrame;
    else if (g.profile != 1 || decoded.value->frame_count != rule.frame_count)
        out.error = SpectralError::IdentityMismatch;
    if (out.error != SpectralError::None)
        return out;
    out.stream = std::move(*decoded.value);
    out.rule = rule;
    out.retained_bytes = decoded.memory.retained_bytes;
    return out;
}

} // namespace pulp::host
