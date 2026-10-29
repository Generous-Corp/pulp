// CV Face Tone WAMv2 factory. The browser owns camera/inference and writes the
// validated face mapping to the ordinary Pulp parameter API.
#include "cv_control_processor.hpp"
#include <memory>
std::unique_ptr<pulp::format::Processor> pulp_wam_make_processor() {
    return pulp::examples::cv_control::create_face_tone();
}
