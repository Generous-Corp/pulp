// CV Face Tone WAMv2 factory. The browser owns camera/inference and sends a
// short MIDI control packet. The shared C++ processor owns mapping and
// additive modulation; the saved parameter remains the automatable base.
#include "cv_control_processor.hpp"
#include <memory>
std::unique_ptr<pulp::format::Processor> pulp_wam_make_processor() {
    return pulp::examples::cv_control::create_face_tone();
}
