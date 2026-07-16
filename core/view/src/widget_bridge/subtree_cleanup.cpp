#include <pulp/view/widget_bridge.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
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

void forget_js_widget_subtree(ScriptEngine& engine,
                              const std::vector<std::string>& ids,
                              bool preserve_js_dom_state) {
    try {
        if (!static_cast<bool>(engine)) return;
        const char* options = preserve_js_dom_state
            ? ", { preserveDomElementState: true }"
            : "";
        engine.evaluate("if (typeof __forgetWidgetCallbacks__ === 'function') "
                        "__forgetWidgetCallbacks__(" + js_string_array_literal(ids) +
                        options + "); void 0;");
    } catch (const std::exception& e) {
        std::cerr << "WidgetBridge subtree callback cleanup error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "WidgetBridge subtree callback cleanup error: unknown exception\n";
    }
}

} // namespace

bool WidgetBridge::claim_pointer_registration(const std::string& id) {
    auto& record = registrations_[id];
    if (record.pointer) return false;
    record.pointer = true;
    return true;
}

bool WidgetBridge::claim_wheel_registration(const std::string& id) {
    auto& record = registrations_[id];
    if (record.wheel) return false;
    record.wheel = true;
    return true;
}

bool WidgetBridge::claim_gesture_registration(const std::string& id,
                                              const std::string& recognizer_key) {
    auto& keys = registrations_[id].gesture_recognizers;
    if (std::find(keys.begin(), keys.end(), recognizer_key) != keys.end()) return false;
    keys.push_back(recognizer_key);
    return true;
}

void WidgetBridge::forget_widget_registrations(const std::string& id) {
    registrations_.erase(id);
}

void WidgetBridge::forget_widget_subtree(View* node, bool preserve_js_dom_state) {
    std::vector<std::string> ids;
    collect_widget_subtree_ids(node, ids);
    if (ids.empty()) return;

    forget_js_widget_subtree(engine_, ids, preserve_js_dom_state);
    for (const auto& id : ids) {
        widgets_.erase(id);
        forget_widget_registrations(id);
    }
    prune_dangling_bindings();
}

void WidgetBridge::forget_widget_event_state(View& view) {
    if (!view.id().empty()) {
        forget_js_widget_subtree(engine_, std::vector<std::string>{view.id()}, false);
        forget_widget_registrations(view.id());
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
