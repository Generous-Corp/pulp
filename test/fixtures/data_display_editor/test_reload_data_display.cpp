// A hot-reloaded editor gets a real level display from the host.
//
// The logic module is thin: it resolves the display registry, the facets'
// typeinfo and the provider's widgets from this host. The editor is opened
// through ViewBridge with the registry cleared first, then the logic is
// reloaded and the open editor rebuilt in place; both editors must hold a
// Meter that the module configured through LevelDisplay.

#include <pulp/format/reload/reloadable_shell.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/data_display.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/inspector_facts.hpp>
#include <pulp/view/view.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace {
pulp::view::View* find_id(pulp::view::View& root, std::string_view id) {
    if (root.id() == id) return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* hit = find_id(*root.child_at(i), id)) return hit;
    return nullptr;
}

std::string describe(const pulp::view::View& view, int depth = 0) {
    std::string out(static_cast<std::size_t>(depth) * 2, ' ');
    out += pulp::view::ViewInspector::type_name(view) + " #" + view.id() + "\n";
    for (std::size_t i = 0; i < view.child_count(); ++i)
        out += describe(*view.child_at(i), depth + 1);
    return out;
}

void require_module_meter(pulp::view::View& root) {
    INFO(describe(root));
    CHECK(find_id(root, "meter-unavailable") == nullptr);
    CHECK(find_id(root, "meter-not-level-display") == nullptr);
    auto* meter = find_id(root, "reload-meter");
    REQUIRE(meter != nullptr);
    CHECK(pulp::view::ViewInspector::type_name(*meter) == "Meter");
    REQUIRE(dynamic_cast<pulp::view::LevelDisplay*>(meter) != nullptr);
    auto* facts = dynamic_cast<pulp::view::InspectorFacts*>(meter);
    REQUIRE(facts != nullptr);
    std::vector<pulp::view::InspectorFacts::Fact> facts_out;
    facts->inspector_facts(facts_out);
    bool saw_rms = false;
    for (const auto& fact : facts_out) {
        if (fact.key == "rms") {
            saw_rms = true;
            CHECK(fact.value == Catch::Approx(0.5));
        }
    }
    CHECK(saw_rms);
}
} // namespace

TEST_CASE("a reloaded editor builds a real level display through the host registry",
          "[format][reload][data-display]") {
    pulp::view::clear_data_displays();
    pulp::format::reload::ReloadableShell shell(RELOAD_LOGIC_METER);
    pulp::state::StateStore store;
    shell.define_parameters(store);
    shell.set_state_store(&store);
    pulp::format::PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = 128;
    prepare.input_channels = 2;
    prepare.output_channels = 2;
    shell.prepare(prepare);

    pulp::format::ViewBridge bridge(shell, store);
    std::string error;
    REQUIRE(bridge.open(&error));
    INFO(error);
    REQUIRE(bridge.view() != nullptr);
    require_module_meter(*bridge.view());

    const auto outcome = shell.reload_now();
    INFO(outcome.detail);
    REQUIRE(outcome.ok());
    REQUIRE(bridge.poll_editor_reload());
    require_module_meter(*bridge.view());
    bridge.close();
    shell.release();
}
