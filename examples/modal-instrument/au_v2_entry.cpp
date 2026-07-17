// ModalInstrument AU v2 instrument entry point.
// Instruments have no audio input, so this routes through MusicDeviceBase
// rather than AUEffectBase. The generated class name must match the CMake
// target name exactly, or the component factory symbol will not resolve.
#include "modal_instrument.hpp"
#include <pulp/format/au_v2_instrument_entry.hpp>

PULP_AU_INSTRUMENT(ModalInstrumentAU, pulp::examples::create_modal_instrument)
