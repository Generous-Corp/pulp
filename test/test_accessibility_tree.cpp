// Cross-platform a11y test harness (workstream 04 slice 4.4).
// Verifies snapshot_accessibility_tree walks the View tree and captures
// role/label/value metadata without needing a platform screen reader.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/accessibility_tree.hpp>
#include <pulp/view/view.hpp>

using namespace pulp::view;

namespace {

class Probe : public View {};

} // namespace

TEST_CASE("snapshot captures role + label + value", "[a11y][harness]") {
    Probe root;
    root.set_access_role(View::AccessRole::group);
    root.set_access_label("Root");

    auto knob = std::make_unique<Probe>();
    knob->set_access_role(View::AccessRole::slider);
    knob->set_access_label("Gain");
    knob->set_access_value("0 dB");
    root.add_child(std::move(knob));

    auto label = std::make_unique<Probe>();
    label->set_access_role(View::AccessRole::label);
    label->set_access_label("Title");
    root.add_child(std::move(label));

    // #192 review: root excluded to match platform bridge output;
    // only descendants reported, child depth resets to 0.
    auto nodes = snapshot_accessibility_tree(root);
    REQUIRE(nodes.size() == 2);
    REQUIRE(nodes[0].role == View::AccessRole::slider);
    REQUIRE(nodes[0].label == "Gain");
    REQUIRE(nodes[0].value == "0 dB");
    REQUIRE(nodes[0].depth == 0);
    REQUIRE(nodes[1].role == View::AccessRole::label);
    REQUIRE(nodes[1].depth == 0);
}

TEST_CASE("count_announceable excludes AccessRole::none", "[a11y][harness]") {
    Probe root;
    // root has role::none by default
    auto a = std::make_unique<Probe>();
    a->set_access_role(View::AccessRole::slider);
    root.add_child(std::move(a));
    auto b = std::make_unique<Probe>();
    // b stays none — invisible to screen readers
    root.add_child(std::move(b));
    auto c = std::make_unique<Probe>();
    c->set_access_role(View::AccessRole::meter);
    root.add_child(std::move(c));
    REQUIRE(count_announceable(root) == 2);
}

TEST_CASE("find_by_role_and_label returns nullptr when absent",
          "[a11y][harness]") {
    Probe root;
    root.set_access_role(View::AccessRole::group);
    root.set_access_label("Root");
    auto knob = std::make_unique<Probe>();
    knob->set_access_role(View::AccessRole::slider);
    knob->set_access_label("Gain");
    root.add_child(std::move(knob));

    REQUIRE(find_by_role_and_label(root, View::AccessRole::slider, "Gain")
            != nullptr);
    REQUIRE(find_by_role_and_label(root, View::AccessRole::slider, "Absent")
            == nullptr);
    REQUIRE(find_by_role_and_label(root, View::AccessRole::meter, "Gain")
            == nullptr);
}
