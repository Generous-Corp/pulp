// The abstract PluginViewHost contract stays in pulp-view-core while the
// platform editor hosts, the frame pipeline and the factory registration live in
// the Pulp-kept pulp-view-tooling target. This binary links pulp::view-core
// only, so it proves a view tree still reaches a host through the seam -- the
// View back-pointer and the damage and frame requests it routes -- without the
// kept layer.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/frame_cost_probe.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#include <memory>

using namespace pulp::view;

TEST_CASE("view-core routes damage to a PluginViewHost through the seam",
          "[view][plugin-host][seam]") {
    View root;
    root.set_bounds({0, 0, 200, 100});
    auto child_owner = std::make_unique<View>();
    View* child = child_owner.get();
    child->set_bounds({10, 10, 20, 20});
    root.add_child(std::move(child_owner));

    FrameCostProbe probe(root, {200, 100});
    REQUIRE(root.plugin_view_host() != nullptr);
    REQUIRE(child->plugin_view_host() == root.plugin_view_host());

    const auto& bounded = probe.measure([&] { child->request_repaint({0, 0, 5, 5}); });
    CHECK(bounded.repaint_requests >= 1);
    CHECK_FALSE(bounded.full_damage);
    CHECK(bounded.has_bounds);

    const auto& full = probe.measure([&] { root.request_repaint(); });
    CHECK(full.full_damage);
}

TEST_CASE("view-core detaches the host from the tree when the seam owner goes",
          "[view][plugin-host][seam]") {
    View root;
    {
        FrameCostProbe probe(root, {10, 10});
        REQUIRE(root.plugin_view_host() != nullptr);
    }
    CHECK(root.plugin_view_host() == nullptr);
}
