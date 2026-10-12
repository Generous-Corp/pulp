// Editor creation through format-view registers the audio displays.
//
// The fixture plug-in's processor objects are linked here and its editor is
// opened through ViewBridge, the path every format adapter takes. The registry
// is cleared first, so a static-initialisation registration cannot stand in
// for the one ViewBridge::open() makes.

#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/data_display.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

std::unique_ptr<pulp::format::Processor> make_data_display_fixture();

namespace {
pulp::view::View* find_anchor(pulp::view::View& root, std::string_view anchor) {
    if (root.anchor_id() == anchor) return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* hit = find_anchor(*root.child_at(i), anchor)) return hit;
    return nullptr;
}
} // namespace

TEST_CASE("a plug-in editor opened through format-view gets a real imported Meter",
          "[format][view][data-display][editor]") {
    pulp::view::clear_data_displays();
    REQUIRE_FALSE(pulp::view::has_data_display("pulp.level"));

    auto processor = make_data_display_fixture();
    pulp::state::StateStore store;
    processor->define_parameters(store);
    processor->set_state_store(&store);

    pulp::format::ViewBridge bridge(*processor, store);
    std::string error;
    REQUIRE(bridge.open(&error));
    INFO(error);
    REQUIRE(bridge.view() != nullptr);

    auto* meter = find_anchor(*bridge.view(), "output-meter");
    REQUIRE(meter != nullptr);
    CHECK(pulp::view::ViewInspector::type_name(*meter) == "Meter");
    CHECK(dynamic_cast<pulp::view::LevelDisplay*>(meter) != nullptr);
    bridge.close();
}
