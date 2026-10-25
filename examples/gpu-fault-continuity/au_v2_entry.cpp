// GPU fault-continuity test plug-in, AU v2 entry point (WP-K).
// Factory function: PulpGpuFaultContinuityAUFactory (the generated Info.plist's factoryFunction).

#include "gpu_fault_continuity.hpp"

#include <pulp/format/au_v2_entry.hpp>

PULP_AU_PLUGIN(PulpGpuFaultContinuityAU, pulp::examples::gpu_fault::create_gpu_fault_continuity)
