#include "gpu_nam_processor.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID GpuNamUID(0x50554C50, 0x476E416D, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(GpuNamUID, "GPU NAM", Steinberg::Vst::PlugType::kFxDistortion,
                  "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                  pulp::examples::create_gpu_nam)
