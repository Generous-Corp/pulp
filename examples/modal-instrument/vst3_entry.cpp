// ModalInstrument VST3 entry point
#include "modal_instrument.hpp"
#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID ModalInstrumentUID(0x50554C50, 0x4D4F4441, 0x4C494E53, 0x00000001);

PULP_VST3_PLUGIN(ModalInstrumentUID, "ModalInstrument",
                 Steinberg::Vst::PlugType::kInstrumentSynth,
                 "Pulp", "1.0.0", "https://github.com/danielraffel/pulp",
                 pulp::examples::create_modal_instrument)
