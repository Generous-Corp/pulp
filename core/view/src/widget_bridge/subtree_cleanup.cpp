#include <pulp/view/widget_bridge.hpp>

#include <choc/text/choc_JSON.h>

#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

namespace {

void collect_widget_subtree_ids(View* node, std::vector<std::string>& ids) {
    if (node == nullptr) {
        return;
    }

    for (std::size_t i = 0; i < node->child_count(); ++i) {
        collect_widget_subtree_ids(node->child_at(i), ids);
    }

    if (!node->id().empty()) {
        ids.push_back(node->id());
    }
}

std::string js_string_literal(std::string_view text) {
    return choc::json::toString(choc::value::createString(std::string(text)), false);
}

std::string js_string_array_literal(const std::vector<std::string>& ids) {
    std::string out = "[";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) out += ",";
        out += js_string_literal(ids[i]);
    }
    out += "]";
    return out;
}

void forget_js_widget_subtree(ScriptEngine& engine, const std::vector<std::string>& ids) {
    try {
        if (!static_cast<bool>(engine)) return;
        engine.evaluate("if (typeof __forgetWidgetCallbacks__ === 'function') "
                        "__forgetWidgetCallbacks__(" + js_string_array_literal(ids) + "); void 0;");
    } catch (const std::exception& e) {
        std::cerr << "WidgetBridge subtree callback cleanup error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "WidgetBridge subtree callback cleanup error: unknown exception\n";
    }
}

} // namespace

void WidgetBridge::forget_widget_subtree(View* node) {
    std::vector<std::string> ids;
    collect_widget_subtree_ids(node, ids);
    if (ids.empty()) return;

    forget_js_widget_subtree(engine_, ids);
    for (const auto& id : ids) {
        widgets_.erase(id);
        pointer_registered_.erase(id);
        wheel_registered_.erase(id);
    }
    prune_dangling_bindings();
}

void WidgetBridge::forget_widget_event_state(View& view) {
    if (!view.id().empty()) {
        forget_js_widget_subtree(engine_, std::vector<std::string>{view.id()});
        pointer_registered_.erase(view.id());
        wheel_registered_.erase(view.id());
    }
    view.on_click = {};
    view.on_pointer_event = {};
    view.on_drag = {};
    view.on_pointer_move = {};
    view.on_gesture_cb = {};
    view.on_context_menu = {};
    view.on_drop = {};
    view.on_hover_enter = {};
    view.on_hover_leave = {};
    view.on_overlay_dismissed = {};
    view.release_overlay();
    view.release_input_focus();
}

} // namespace pulp::view
