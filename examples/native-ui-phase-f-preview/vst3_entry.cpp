#include "chainer_native_ui_processor.hpp"

#include <pulp/format/vst3_entry.hpp>

static const Steinberg::FUID PulpChainerNativeUiUID(
    0x50554C50, 0x4E554950, 0x00000001, 0x00000001);

PULP_VST3_PLUGIN(PulpChainerNativeUiUID,
                 "PulpChainerNativeUi",
                 Steinberg::Vst::PlugType::kFx,
                 "Pulp",
                 "0.1.0",
                 "https://github.com/Generous-Corp/pulp",
                 pulp::examples::create_chainer_native_ui_processor)
