// SPDX-License-Identifier: MIT
#include "observer_script_lane.hpp"

#include <pulp/state/store.hpp>
#include <pulp/view/design_codegen.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>

namespace pulp::import_design {

struct ObservedScript::State {
    // Declared in construction order and destroyed in reverse: the bridge goes
    // first, while the views it registered and the engine it calls still exist.
    pulp::view::View host;
    pulp::view::ScriptEngine engine;
    pulp::state::StateStore store;
    std::unique_ptr<pulp::view::WidgetBridge> bridge;
};

ObservedScript::ObservedScript(std::unique_ptr<State> state) : state_(std::move(state)) {}

ObservedScript::~ObservedScript() = default;

std::unique_ptr<ObservedScript> ObservedScript::run(const pulp::view::DesignIR& ir,
                                                    const std::filesystem::path& asset_directory,
                                                    float width, float height) {
    auto state = std::make_unique<State>();
    state->host.set_bounds({0.0f, 0.0f, width, height});
    state->bridge =
        std::make_unique<pulp::view::WidgetBridge>(state->engine, state->host, state->store);
    state->bridge->set_script_base_dir(asset_directory);
    state->bridge->load_script(pulp::view::generate_pulp_js(ir, pulp::view::CodeGenOptions{}));
    // The program names its views with bridge ids ("div4") and binds each to
    // its source anchor with setAnchor(). The native materializer names a view
    // by that anchor, and the lab joins a layout snapshot to the capture by
    // view id, so the observed ids are relabelled with the anchors once the
    // program has run. The program itself is unchanged.
    const auto relabel = [](auto& self, pulp::view::View& view) -> void {
        if (!view.anchor_id().empty())
            view.set_id(view.anchor_id());
        for (std::size_t i = 0; i < view.child_count(); ++i)
            self(self, *view.child_at(i));
    };
    relabel(relabel, state->host);
    return std::unique_ptr<ObservedScript>(new ObservedScript(std::move(state)));
}

pulp::view::View& ObservedScript::root() {
    return state_->host;
}

} // namespace pulp::import_design
