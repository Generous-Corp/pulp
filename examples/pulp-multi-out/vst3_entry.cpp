// pulp-multi-out — VST3 INSTRUMENT entry point.
// The descriptor's eight output buses become eight VST3 audio output buses.
#include "multi_out_synth.hpp"
#include <pulp/format/vst3_entry.hpp>

// Unique ID — stable across versions, never change.
static const Steinberg::FUID PulpMultiOutUID(0x50554C50, 0x4D4F5554, 0x00000001,
                                             0x00000001);

PULP_VST3_PLUGIN(PulpMultiOutUID, "PulpMultiOut",
                 Steinberg::Vst::PlugType::kInstrumentSynth,
                 "Pulp", "0.1.0", "https://github.com/Generous-Corp/pulp",
                 pulp::examples::multi_out::create_multi_out_synth)
