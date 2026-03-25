#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/auto_ui.hpp>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

TEST_CASE("AutoUi builds from parameter store", "[view][auto_ui]") {
    StateStore store;
    store.add_parameter({1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}});
    store.add_parameter({2, "Mix", "%", {0.0f, 100.0f, 100.0f}});
    store.add_parameter({3, "Bypass", "", {0.0f, 1.0f, 0.0f, 1.0f}}); // step=1 → toggle

    auto root = AutoUi::build(store);
    REQUIRE(root != nullptr);

    // Should have title label + row with 3 params
    REQUIRE(root->child_count() == 2);
}

TEST_CASE("AutoUi sync updates widgets", "[view][auto_ui]") {
    StateStore store;
    store.add_parameter({1, "Gain", "dB", {-60.0f, 12.0f, 0.0f}});

    auto root = AutoUi::build(store);
    REQUIRE(root != nullptr);

    // Change param value
    store.set_normalized(1, 0.8f);
    AutoUi::sync(*root, store);

    // Find the knob and check value
    std::function<Knob*(View&)> find_knob = [&](View& v) -> Knob* {
        if (auto* k = dynamic_cast<Knob*>(&v)) {
            if (k->id() == "Gain") return k;
        }
        for (size_t i = 0; i < v.child_count(); ++i) {
            if (auto* k = find_knob(*v.child_at(i))) return k;
        }
        return nullptr;
    };

    auto* knob = find_knob(*root);
    REQUIRE(knob != nullptr);
    REQUIRE_THAT(knob->value(), WithinAbs(0.8, 0.01));
}
