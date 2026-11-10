// One translation unit per example: their headers declare overlapping names.
#include "example_param_ids.hpp"

#include "../../examples/PulpDrums/pulp_drums.hpp"

namespace pulp::test {
ExampleParams params_of_drums() {
    auto processor = pulp::examples::create_pulp_drums();
    state::StateStore store;
    processor->define_parameters(store);
    return {"drums", store.param_count(), store.duplicate_registrations()};
}
}  // namespace pulp::test
