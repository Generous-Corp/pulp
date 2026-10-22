#pragma once

// The one place the plug-in reaches the audio provider's fault seam.
//
// Audio-device faults (forced device loss at the three submission points, a
// delayed completion, a hung terminal) are injected by WP-A2's
// `gpu_audio_fault_inject` seam, which forwards to the shared-I/O provider's
// test fault hooks. Until that seam is on this branch every audio fault is
// refused with a reason the harness records; the plug-in never fakes a fault
// it cannot inject. render_force_loss does not come through here: it is the
// editor's render device, reached through the surface's device accessor.

#include "gpu_fault_continuity.hpp"

#include <pulp/gpu_audio/gpu_audio_transport.hpp>

#include <string>

namespace pulp::examples::gpu_fault {

struct FaultSeamResult {
    bool accepted = false;
    std::string detail;
};

inline std::string audio_fault_unavailable_reason(FaultKind kind) {
    return std::string("audio fault ") + fault_name(kind) +
           " needs the WP-A2 gpu_audio_fault_inject seam, which is not on this branch";
}

inline FaultSeamResult inject_audio_fault(gpu_audio::GpuAudioTransport&, FaultKind kind) {
    return {false, audio_fault_unavailable_reason(kind)};
}

inline bool audio_fault_seam_available() noexcept {
    return false;
}

} // namespace pulp::examples::gpu_fault
