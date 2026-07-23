#include "sequence_demo_processor.hpp"

#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PulpSequenceUID(0x50554C50, 0x53455100, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpSequenceUID, "Pulp Sequence", Steinberg::Vst::PlugType::kInstrumentSynth,
                 "Pulp", "1.0.0", "https://github.com/Generous-Corp/pulp",
                 pulp::examples::create_pulp_sequence)
