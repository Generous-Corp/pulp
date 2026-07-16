#include "bridge_dispatch.hpp"

#include <choc/text/choc_JSON.h>

#include <exception>
#include <iostream>

namespace pulp::view {

std::string js_string_literal(std::string_view text) {
    return choc::json::toString(choc::value::createString(std::string(text)), false);
}

void safe_dispatch_eval(const std::shared_ptr<std::atomic<bool>>& alive,
                        ScriptEngine* engine,
                        const std::string& js,
                        const char* context) {
    if (!alive || !alive->load(std::memory_order_acquire) || engine == nullptr) return;
    try {
        if (!static_cast<bool>(*engine)) return;
        engine->evaluate(js);
        // Pump microtasks so React setState commits (and any queueMicrotask /
        // Promise.then continuations scheduled by the handler) before the next
        // event arrives. Without this, drag-style interactions see stale state
        // on the immediately-following pointermove and silently bail.
        engine->pump_message_loop();
    } catch (const std::exception& e) {
        std::cerr << "WidgetBridge " << context << " error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "WidgetBridge " << context << " error: unknown exception\n";
    }
}

void safe_dispatch_eval(ScriptEngine& engine, const std::string& js, const char* context) {
    // The no-flag path targets a known-valid engine reference; delegate through
    // an always-alive flag so both overloads share one implementation.
    static const auto always_alive = std::make_shared<std::atomic<bool>>(true);
    safe_dispatch_eval(always_alive, &engine, js, context);
}

void dispatch_event(const std::shared_ptr<std::atomic<bool>>& alive,
                    ScriptEngine* engine,
                    std::string_view id,
                    const std::string& event_name,
                    std::string_view payload_expr) {
    std::string js = "__dispatch__(" + js_string_literal(id) + ", '" + event_name + "', ";
    js.append(payload_expr.data(), payload_expr.size());
    js += ")";
    safe_dispatch_eval(alive, engine, js, event_name.c_str());
}

void dispatch_event(ScriptEngine& engine,
                    std::string_view id,
                    const std::string& event_name,
                    std::string_view payload_expr) {
    static const auto always_alive = std::make_shared<std::atomic<bool>>(true);
    dispatch_event(always_alive, &engine, id, event_name, payload_expr);
}

} // namespace pulp::view
