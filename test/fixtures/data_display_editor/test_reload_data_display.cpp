// A hot-reloaded editor gets a real level display from the host.
//
// The logic module is thin: it resolves the importer, the display registry,
// the facets' typeinfo and the provider's widgets from this host. The editor is
// opened through ViewBridge with the registry cleared first, then the logic is
// reloaded and the open editor rebuilt in place; both editors must hold an
// imported Meter that the module configured through LevelDisplay.

#include <pulp/format/reload/reloadable_shell.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/data_display.hpp>
#include <pulp/view/design_import.hpp>
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
    if (root.id() == id)
        return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* hit = find_id(*root.child_at(i), id))
            return hit;
    return nullptr;
}

std::string describe(const pulp::view::View& view, int depth = 0) {
    std::string out(static_cast<std::size_t>(depth) * 2, ' ');
    out += pulp::view::ViewInspector::type_name(view) + " #" + view.id() + "\n";
    for (std::size_t i = 0; i < view.child_count(); ++i)
        out += describe(*view.child_at(i), depth + 1);
    return out;
}

// The module renames the imported meter only after its own LevelDisplay cast
// succeeded and it set the levels through the interface.
void require_module_meter(pulp::view::View& root) {
    INFO(describe(root));
    auto* meter = find_id(root, "module-configured-level");
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

TEST_CASE("a reloaded editor imports a real level display through the host",
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

    // The same import made by the host agrees with the module's. This is also
    // what keeps the importer in the host image for the module to resolve.
    pulp::view::DesignIR ir;
    ir.root.type = "frame";
    pulp::view::IRNode meter;
    meter.type = "frame";
    meter.stable_anchor_id = "host-meter";
    meter.audio_widget = pulp::view::AudioWidgetType::meter;
    ir.root.children.push_back(meter);
    auto host_root = pulp::view::build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(host_root != nullptr);
    REQUIRE(host_root->child_count() == 1);
    CHECK(pulp::view::ViewInspector::type_name(*host_root->child_at(0)) == "Meter");

    bridge.close();
    shell.release();
}
