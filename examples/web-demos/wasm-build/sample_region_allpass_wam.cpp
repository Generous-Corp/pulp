#include "allpass_processor.hpp"

std::unique_ptr<pulp::format::Processor> pulp_wam_make_processor() {
    return pulp::examples::create_sample_region_allpass();
}
