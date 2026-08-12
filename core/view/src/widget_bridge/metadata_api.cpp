// widget_bridge/metadata_api.cpp - metadata registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/canvas_widget.hpp>
#include "api_registry.hpp"

#include <cmath>
#include <string>
#include <utility>

namespace pulp::view {

namespace {

View* unique_anchor(View& root, std::string_view anchor, int& matches) {
    View* result = nullptr;
    if (root.anchor_id() == anchor) {
        ++matches;
        result = &root;
    }
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        if (auto* found = unique_anchor(*root.child_at(i), anchor, matches))
            result = found;
    }
    return result;
}

CanvasWidget* canvas_at(View& root, std::size_t wanted, std::size_t& seen) {
    if (auto* canvas = dynamic_cast<CanvasWidget*>(&root)) {
        if (seen++ == wanted) return canvas;
    }
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* found = canvas_at(*root.child_at(i), wanted, seen))
            return found;
    return nullptr;
}

bool bind_canvas_program(View& root, std::string anchor, CanvasWidget* source) {
    int matches = 0;
    auto* target_view = unique_anchor(root, anchor, matches);
    if (matches != 1 || !target_view || !source || target_view == source)
        return false;
    // Copy the bridge-safe callbacks instead of retaining either the source
    // View or WidgetBridge. The copied closures already carry the shared
    // BridgeCallbackState used by every registered pointer callback, so they
    // become no-ops after realm teardown. This also survives the hidden
    // behavior tree being replaced between pointer deliveries without leaving
    // a dangling View pointer on the visible DesignIR surface.
    target_view->on_dom_pointer_event = source->on_dom_pointer_event;
    target_view->on_dom_pointer_move_event = source->on_dom_pointer_move_event;
    return true;
}

} // namespace

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
                self.retire_removed_widget(std::move(removed));
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

    // bindCanvasBehaviorAt(anchor, sourceRootId, canvasIndex) -> bool
    //
    // A Chromium-captured canvas bitmap remains the visual authority until
    // its executable native Canvas2D program independently passes the visual
    // parity gate. This binding forwards input to the live program without
    // replacing the accepted captured paint with a partial command stream.
    register_bridge_function(api, "bindCanvasBehaviorAt", [&self](
        choc::javascript::ArgumentList args) {
        const auto anchor = args.get<std::string>(0, "");
        const auto source_root_id = args.get<std::string>(1, "");
        const auto raw_index = args.get<double>(2, -1.0);
        if (anchor.empty() || source_root_id.empty() ||
            !std::isfinite(raw_index) || raw_index < 0.0 ||
            std::floor(raw_index) != raw_index ||
            // Browser capture bounds materialized canvases to 256. Keeping the
            // same public bound makes the double-to-size_t conversion total on
            // every supported architecture.
            raw_index > 255.0)
            return choc::value::createBool(false);
        auto* source_root = self.widget(source_root_id);
        std::size_t seen = 0;
        auto* source = source_root
            ? canvas_at(*source_root, static_cast<std::size_t>(raw_index), seen)
            : nullptr;
        return choc::value::createBool(
            source && bind_canvas_program(self.root_, anchor, source));
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
