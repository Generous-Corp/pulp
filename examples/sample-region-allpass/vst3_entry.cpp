#include "allpass_processor.hpp"
#include <pulp/format/vst3_entry.hpp>

// Stable VST3 identity for com.pulp.sample-region-allpass.  Do not regenerate
// this FUID: hosts persist it in project state and automation envelopes.
static const Steinberg::FUID SampleRegionAllpassUID(
    0x50554C50, 0x53414D50, 0x52454749, 0x4F4E414C);

PULP_VST3_PLUGIN(SampleRegionAllpassUID, "Sample Region Allpass",
                Steinberg::Vst::PlugType::kFx, "Pulp", "1.0.0",
                "https://github.com/Generous-Corp/pulp",
                pulp::examples::create_sample_region_allpass)
