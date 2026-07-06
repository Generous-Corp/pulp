// widget_bridge/metadata_api.cpp - metadata registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <string>
#include <unordered_map>
#include <utility>

namespace pulp::view {

namespace {

void erase_widget_subtree(std::unordered_map<std::string, View*>& widgets, View* node) {
    if (node == nullptr) {
        return;
    }

    for (size_t i = 0; i < node->child_count(); ++i) {
        erase_widget_subtree(widgets, node->child_at(i));
    }

    if (!node->id().empty()) {
        widgets.erase(node->id());
    }
}

} // namespace

void WidgetBridge::register_metadata_removal_api() {
    BridgeApiContext api{engine_};

    // removeWidget(id)
    register_bridge_function(api, "removeWidget", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        if (auto* w = widget(id)) {
            View* parent = w->parent();
            if (parent) {
                auto removed = parent->remove_child(w);
                erase_widget_subtree(widgets_, removed.get());
                // Drop param/meter bindings for any id in the removed subtree so
                // a later widget reusing an id is not silently re-bound.
                prune_dangling_bindings();
            }
        }
        return choc::value::Value();
    });
}

void WidgetBridge::register_metadata_source_api() {
    BridgeApiContext api{engine_};

    register_bridge_function(api, "setAnchor", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto anchor = args.get<std::string>(1, "");
        if (auto* v = widget(id)) v->set_anchor_id(std::move(anchor));
        return choc::value::Value();
    });

    register_bridge_function(api, "setSource", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto file = args.get<std::string>(1, "");
        auto line = static_cast<int>(args.get<double>(2, 0.0));
        auto col = static_cast<int>(args.get<double>(3, 0.0));
        if (file.empty()) return choc::value::Value();
        if (auto* v = widget(id))
            v->set_source_loc({std::move(file), line, col});
        return choc::value::Value();
    });
}

void WidgetBridge::register_metadata_computed_api() {
    BridgeApiContext api{engine_};

    // getComputedValue(id, prop) -> string
    register_bridge_function(api, "getComputedValue", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto prop = args.get<std::string>(1, "");
        auto* v = widget(id);
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
