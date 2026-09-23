#include "control_sample_region_allpass.hpp"
int main() {
    return pulp::examples::control_allpass::run(&pulp::examples::create_sample_region_allpass,
                                                &pulp::examples::control_allpass::editable_target,
                                                false);
}
