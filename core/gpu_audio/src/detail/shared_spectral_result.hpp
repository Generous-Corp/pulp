#pragma once

#include "shared_io_program_session.hpp"
#include <cstring>
#include <pulp/gpu_audio/gpu_spectral_mask.hpp>

namespace pulp::gpu_audio::detail {

// The persistent spectral history becomes unusable after the first physical
// failure. Continue retiring every result, but never expose later output from
// that epoch even when its individual dispatch completed successfully.
inline std::optional<GpuSpectralMaskSession::Result>
receive_shared_spectral_result(SharedIoProgramSession& session, std::uint64_t epoch, bool& failed,
                               std::span<float> output, std::size_t samples,
                               std::uint64_t& copied_bytes) noexcept {
    if (output.size() < samples)
        return std::nullopt;
    auto completion = session.pop_completion();
    if (!completion)
        return std::nullopt;
    GpuSpectralMaskSession::Result result{epoch, completion->token.slot.stream_sequence, false,
                                          completion->late};
    if (failed || completion->status != SharedIoArena::CompletionStatus::RetiredSuccess) {
        failed = true;
        (void)session.discard_completion(*completion);
        return result;
    }
    auto lease = session.acquire_output(*completion);
    if (!lease || lease->bytes.size() < samples * sizeof(float)) {
        failed = true;
        if (lease)
            (void)session.release_output({lease->token});
        else
            (void)session.discard_completion(*completion);
        return result;
    }
    std::memcpy(output.data(), lease->bytes.data(), samples * sizeof(float));
    copied_bytes += samples * sizeof(float);
    result.delivered = session.release_output({lease->token});
    if (!result.delivered)
        failed = true;
    return result;
}

} // namespace pulp::gpu_audio::detail
