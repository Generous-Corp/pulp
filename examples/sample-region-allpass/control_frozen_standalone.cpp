#include "control_sample_region_allpass.hpp"
int main() {
    return pulp::examples::control_allpass::run(
        &pulp::examples::control_allpass::create_frozen,
        &pulp::examples::control_allpass::frozen_target, true);
}
