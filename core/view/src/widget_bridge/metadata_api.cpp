// widget_bridge/metadata_api.cpp - metadata registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <string>
#include <utility>

namespace pulp::view {

void BridgeRegistrars::register_metadata_removal_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // removeWidget(id)
    register_bridge_function(api, "removeWidget", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        if (auto* w = self.widget(id)) {
            View* parent = w->parent();
            if (parent) {
                auto removed = parent->remove_child(w);
                self.forget_widget_subtree(removed.get());
            }
        }
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_metadata_source_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "setAnchor", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto anchor = args.get<std::string>(1, "");
        if (auto* v = self.widget(id)) v->set_anchor_id(std::move(anchor));
        return choc::value::Value();
    });

    register_bridge_function(api, "setSource", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto file = args.get<std::string>(1, "");
        auto line = static_cast<int>(args.get<double>(2, 0.0));
        auto col = static_cast<int>(args.get<double>(3, 0.0));
        if (file.empty()) return choc::value::Value();
        if (auto* v = self.widget(id))
            v->set_source_loc({std::move(file), line, col});
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_metadata_computed_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // getComputedValue(id, prop) -> string
    register_bridge_function(api, "getComputedValue", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto prop = args.get<std::string>(1, "");
        auto* v = self.widget(id);
        if (!v) return choc::value::createString("");
        if (prop == "width") return choc::value::createString(std::to_string(v->bounds().width) + "px");
        if (prop == "height") return choc::value::createString(std::to_string(v->bounds().height) + "px");
        if (prop == "opacity") return choc::value::createString(std::to_string(v->opacity()));
        if (prop == "display") return choc::value::createString(v->visible() ? "flex" : "none");
        if (prop == "visibility") return choc::value::createString(v->visible() ? "visible" : "hidden");
        return choc::value::createString("");
    });
}

} // namespace pulp::view
