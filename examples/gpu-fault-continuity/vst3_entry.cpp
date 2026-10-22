#include "gpu_fault_continuity.hpp"
#include <pulp/format/vst3_entry.hpp>

// Unique ID — stable across versions, never change.
static const Steinberg::FUID GpuFaultContinuityUID(0x50554C50, 0x47464331, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(GpuFaultContinuityUID, "Pulp GPU Fault Continuity", Steinberg::Vst::PlugType::kFx,
                 "Pulp", "0.1.0", "https://github.com/Generous-Corp/pulp",
                 pulp::examples::gpu_fault::create_gpu_fault_continuity)
