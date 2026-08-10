#include <pulp/view/widget_bridge.hpp>
#include "bridge_dispatch.hpp"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

namespace {

void collect_widget_subtree_ids(
    View* node, std::vector<std::string>& ids,
    std::vector<std::uint64_t>& instance_ids,
    const WidgetBridge::DeadlineCheck& deadline_check) {
    if (deadline_check) deadline_check();
    if (node == nullptr) {
        return;
    }

    for (std::size_t i = 0; i < node->child_count(); ++i) {
        collect_widget_subtree_ids(
            node->child_at(i), ids, instance_ids, deadline_check);
    }

    instance_ids.push_back(node->import_binding_instance_id());
    if (!node->id().empty()) {
        ids.push_back(node->id());
    }
}

std::string js_string_literal(std::string_view text) {
    return choc::json::toString(choc::value::createString(std::string(text)), false);
}

std::string js_string_array_literal(
    const std::vector<std::string>& ids,
    const WidgetBridge::DeadlineCheck& deadline_check) {
    std::string out = "[";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (deadline_check) deadline_check();
        if (i > 0) out += ",";
        out += js_string_literal(ids[i]);
    }
    out += "]";
    return out;
}

void forget_js_widget_subtree(ScriptEngine& engine,
                              const std::vector<std::string>& ids,
                              bool preserve_js_dom_state,
                              const WidgetBridge::DeadlineCheck& deadline_check) {
    const auto ids_literal = js_string_array_literal(ids, deadline_check);
    if (deadline_check) deadline_check();
    try {
        if (!static_cast<bool>(engine)) return;
        const char* options = preserve_js_dom_state
            ? ", { preserveDomElementState: true }"
            : "";
        engine.evaluate("if (typeof __forgetWidgetCallbacks__ === 'function') "
                        "__forgetWidgetCallbacks__(" + ids_literal +
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

void WidgetBridge::forget_widget_subtree(
    View* node, bool preserve_js_dom_state,
    const DeadlineCheck& deadline_check, bool notify_js) {
    invalidate_cached_subtrees_everywhere({node});
    std::vector<std::string> ids;
    std::vector<std::uint64_t> instance_ids;
    collect_widget_subtree_ids(node, ids, instance_ids, deadline_check);

    std::sort(instance_ids.begin(), instance_ids.end());
    if (deadline_check) deadline_check();
    owned_widgets_.erase(
        std::remove_if(
            owned_widgets_.begin(), owned_widgets_.end(),
            [&](const auto& owned) {
                return std::binary_search(
                    instance_ids.begin(), instance_ids.end(),
                    owned.instance_id);
            }),
        owned_widgets_.end());

    if (notify_js && !ids.empty())
        forget_js_widget_subtree(
            engine_, ids, preserve_js_dom_state, deadline_check);
    for (const auto& id : ids) {
        if (deadline_check) deadline_check();
        release_param_gesture_route(id);
        widgets_.erase(id);
        forget_widget_registrations(id);
    }
    prune_dangling_bindings();
}

void WidgetBridge::forget_widget_event_state(View& view) {
    if (!view.id().empty()) {
        forget_js_widget_subtree(
            engine_, std::vector<std::string>{view.id()}, false, {});
        forget_widget_registrations(view.id());
    }
    view.on_click = {};
    view.on_pointer_event = {};
    view.on_dom_wheel_event = {};
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

void WidgetBridge::retire_removed_widget(std::unique_ptr<View> widget) {
    if (callback_alive_)
        callback_alive_->retire(std::move(widget));
}

} // namespace pulp::view
