#pragma once

// The one place the plug-in reaches the audio provider's faults.
//
// The real DawnSharedIoProvider's fault hooks are construction-time Options,
// and WP-A2's `dev.pulp.gpu-audio/fault.inject@1` op arms only fault targets
// that wire their own arm function; a plug-in inside REAPER is also not
// reachable by the control broker. So this test plug-in arms its faults
// through the Dawn proc-table interposer in gpu_fault_interposer.cpp, which
// faults the running provider at the provider's own call sites.
// render_force_loss does not come through here: it is the editor's render
// device, reached through the surface's device accessor.

#include "gpu_fault_continuity.hpp"
#include "gpu_fault_interposer.hpp"

#include <string>

namespace pulp::examples::gpu_fault {

struct FaultSeamResult {
    bool accepted = false;
    std::string detail;
};

inline std::string audio_fault_unavailable_reason(FaultKind kind) {
    return std::string("audio fault ") + fault_name(kind) +
           " needs the provider fault interposer, which is not installed (no Dawn headers, or "
           "the shared-I/O provider did not prepare)";
}

inline FaultSeamResult inject_audio_fault(FaultKind kind, std::uint32_t delay_ms) {
    FaultSeamResult result;
    result.accepted = arm_provider_fault(kind, delay_ms, result.detail);
    return result;
}

inline bool audio_fault_seam_available() noexcept {
    return provider_fault_interposer_installed();
}

} // namespace pulp::examples::gpu_fault
