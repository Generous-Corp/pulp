#include "cv_control_processor.hpp"
#include <pulp/format/vst3_entry.hpp>
static const Steinberg::FUID CvFaceToneUID(0x43564654, 0x4F4E4501, 0x00000001, 0x00000001);
PULP_VST3_PLUGIN(CvFaceToneUID, "CV Face Tone", Steinberg::Vst::PlugType::kInstrumentSynth, "Pulp",
                 "0.1.0", "https://github.com/Generous-Corp/pulp",
                 pulp::examples::cv_control::create_face_tone)
