// One translation unit per example: their headers declare overlapping names.
#include "example_param_ids.hpp"

#include "../../examples/pulp-gain/pulp_gain.hpp"

namespace pulp::test {
ExampleParams params_of_gain() {
    auto processor = pulp::examples::create_pulp_gain();
    state::StateStore store;
    processor->define_parameters(store);
    return {"gain", store.param_count(), store.duplicate_registrations()};
}
}  // namespace pulp::test
