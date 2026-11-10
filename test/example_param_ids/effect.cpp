// One translation unit per example: their headers declare overlapping names.
#include "example_param_ids.hpp"

#include "../../examples/pulp-effect/pulp_effect.hpp"

namespace pulp::test {
ExampleParams params_of_effect() {
    auto processor = pulp::examples::create_pulp_effect();
    state::StateStore store;
    processor->define_parameters(store);
    return {"effect", store.param_count(), store.duplicate_registrations()};
}
} // namespace pulp::test
