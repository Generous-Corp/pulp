#include <catch2/catch_test_macros.hpp>

#include "example_param_ids/example_param_ids.hpp"

#include <pulp/state/store.hpp>

// Every parameter ID a processor registers must be its own: the store refuses a
// second registration under a used ID, so a processor that does it loses a
// parameter. This builds each shipped example whose factory is in a header and
// requires zero refused registrations.
TEST_CASE("shipped example processors register no duplicate parameter id", "[state][examples]") {
    for (const auto& example : {pulp::test::params_of_gain(), pulp::test::params_of_effect(),
                                pulp::test::params_of_drums()}) {
        DYNAMIC_SECTION(example.name) {
            CHECK(example.count > 0);
            CHECK(example.refused == 0);
        }
    }
}

TEST_CASE("the example duplicate check can fail", "[state][examples]") {
    // Control: the counter the check reads does move when an ID is reused.
    pulp::state::StateStore store;
    pulp::state::ParamInfo p{.id = 3, .name = "A", .range = {0.0f, 1.0f, 0.5f, 0.0f}};
    REQUIRE(store.add_parameter(p));
    REQUIRE_FALSE(store.add_parameter(p));
    REQUIRE(store.duplicate_registrations() == 1);
}
