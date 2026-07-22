// PulpTempoSampler VST3 entry point.

#include "pulp_tempo_sampler.hpp"
#include <pulp/format/vst3_entry.hpp>

// Unique ID — stable across versions, never change.
static const Steinberg::FUID PulpTempoSamplerUID(0x50554C50, 0x54454D50, 0x53414D50, 0x00000001);

PULP_VST3_PLUGIN(PulpTempoSamplerUID, "PulpTempoSampler",
                 Steinberg::Vst::PlugType::kInstrumentSynth,
                 "Pulp", pulp::examples::kPulpTempoSamplerVersion,
                 "https://github.com/Generous-Corp/pulp",
                 pulp::examples::create_pulp_tempo_sampler)
