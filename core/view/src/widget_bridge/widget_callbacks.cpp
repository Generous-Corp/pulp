#include <pulp/view/widget_bridge.hpp>

#include <pulp/view/ui_components.hpp>
#include <pulp/view/virtual_list.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulp::view {

namespace {

void safe_dispatch_eval(const std::shared_ptr<std::atomic<bool>>& alive,
                        ScriptEngine* engine,
                        const std::string& js,
                        const char* context) {
    if (!alive || !alive->load(std::memory_order_acquire) || engine == nullptr) return;
    try {
        if (!static_cast<bool>(*engine)) return;
        engine->evaluate(js);
        engine->pump_message_loop();
    } catch (const std::exception& e) {
        std::cerr << "WidgetBridge " << context << " error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "WidgetBridge " << context << " error: unknown exception\n";
    }
}

std::string js_string_literal(std::string_view text) {
    return choc::json::toString(choc::value::createString(std::string(text)), false);
}

void dispatch_virtual_list_row_release(const std::shared_ptr<std::atomic<bool>>& alive,
                                       ScriptEngine* engine,
                                       const std::string& id_literal,
                                       const std::string& row_id) {
    safe_dispatch_eval(alive, engine,
        "__dispatch__(" + id_literal + ", 'releaserow', { rowId: " +
        js_string_literal(row_id) + " })",
        "virtual list releaserow");
}

} // namespace

void WidgetBridge::wire_callbacks(const std::string& id, View* w) {
    auto alive = callback_alive_;
    auto* engine = &engine_;
    if (auto* k = dynamic_cast<Knob*>(w)) {
        k->on_change = [alive, engine, id](float v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::to_string(v) + ")", "knob change");
        };
    } else if (auto* f = dynamic_cast<Fader*>(w)) {
        f->on_change = [alive, engine, id](float v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::to_string(v) + ")", "fader change");
        };
    } else if (auto* t = dynamic_cast<Toggle*>(w)) {
        t->on_toggle = [alive, engine, id](bool v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'toggle', " + std::string(v?"1":"0") + ")", "toggle");
        };
    } else if (auto* r = dynamic_cast<RangeSlider*>(w)) {
        // HTML <input type="range"> change event. The payload is the
        // post-quantisation value, not normalized, so JS callers see the same
        // number they handed us via setValue/setMin/setMax/setStep.
        r->on_change = [alive, engine, id](float v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::to_string(v) + ")", "range slider change");
        };
    } else if (auto* c = dynamic_cast<ComboBox*>(w)) {
        // Mirror createCombo's inline wiring so a `<combo>`/`<select>` tag
        // routed through __domAppend dispatches the same `select` event as the
        // factory path.
        c->on_change = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'select', " + std::to_string(idx) + ")", "combo select");
        };
    } else if (auto* cb = dynamic_cast<Checkbox*>(w)) {
        // Mirror createCheckbox's inline `change` wiring.
        cb->on_change = [alive, engine, id](bool v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::string(v?"1":"0") + ")", "checkbox change");
        };
    } else if (auto* lb = dynamic_cast<ListBox*>(w)) {
        // Mirror createListBox's inline select/activate wiring.
        lb->on_select = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'select', " + std::to_string(idx) + ")", "list select");
        };
        lb->on_activate = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'activate', " + std::to_string(idx) + ")", "list activate");
        };
    } else if (auto* vl = dynamic_cast<VirtualList*>(w)) {
        const auto id_literal = js_string_literal(id);
        auto* widgets = &widgets_;
        auto row_bindings = std::make_shared<std::unordered_map<std::string, std::size_t>>();
        vl->set_row_releaser([this, alive, engine, id_literal, row_bindings](View& row) {
            if (!alive || !alive->load(std::memory_order_acquire)) return;
            row_bindings->erase(row.id());
            dispatch_virtual_list_row_release(alive, engine, id_literal, row.id());
            if (!alive || !alive->load(std::memory_order_acquire)) return;
            forget_widget_subtree(&row);
        });
        vl->set_row_factory([widgets, alive, id](std::size_t slot) {
            auto row = std::make_unique<View>();
            row->set_id(id + "__row_" + std::to_string(slot));
            if (alive && alive->load(std::memory_order_acquire)) {
                auto* row_ptr = row.get();
                (*widgets)[row->id()] = row_ptr;
            }
            return row;
        });
        vl->set_row_binder([this, alive, engine, id_literal, row_bindings](View& row, std::size_t index) {
            const auto row_id = row.id();
            if (row_bindings->find(row_id) != row_bindings->end()) {
                forget_widget_event_state(row);
                while (row.child_count() > 0) {
                    auto* child = row.child_at(row.child_count() - 1);
                    auto removed = row.remove_child(child);
                    forget_widget_subtree(removed.get());
                }
            }
            (*row_bindings)[row_id] = index;
            safe_dispatch_eval(alive, engine,
                "__dispatch__(" + id_literal + ", 'bindrow', { rowId: " +
                js_string_literal(row_id) + ", index: " + std::to_string(index) + " })",
                "virtual list bindrow");
        });
        vl->on_selection_changed([alive, engine, id_literal](const std::vector<std::size_t>& selection) {
            std::string payload = "[";
            for (std::size_t i = 0; i < selection.size(); ++i) {
                if (i > 0) payload += ",";
                payload += std::to_string(selection[i]);
            }
            payload += "]";
            safe_dispatch_eval(alive, engine, "__dispatch__(" + id_literal + ", 'change', " + payload + ")",
                               "virtual list change");
        });
        vl->on_row_activated([alive, engine, id_literal](std::size_t index) {
            safe_dispatch_eval(alive, engine, "__dispatch__(" + id_literal + ", 'activate', " +
                               std::to_string(index) + ")", "virtual list activate");
        });
    }
}

} // namespace pulp::view
