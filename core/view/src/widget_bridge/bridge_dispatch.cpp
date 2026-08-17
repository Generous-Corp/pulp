#include "bridge_dispatch.hpp"

#include <choc/text/choc_JSON.h>

#include <exception>
#include <iostream>

namespace pulp::view {

void BridgeCallbackState::enter_callback() noexcept {
    if (callback_depth_ == 0 && collectable_widgets_)
        collectable_widgets_->clear();
    ++callback_depth_;
}

void BridgeCallbackState::leave_callback() noexcept {
    if (callback_depth_ == 0) return;
    --callback_depth_;
    if (callback_depth_ == 0 && retired_widgets_ && collectable_widgets_) {
        collectable_widgets_->clear();
        collectable_widgets_->swap(*retired_widgets_);
    }
}

void BridgeCallbackState::retire(std::unique_ptr<View> widget) {
    if (!widget) return;
    if (callback_depth_ == 0 || !retired_widgets_) return;
    retired_widgets_->push_back(std::move(widget));
}

void BridgeCallbackState::detach_retirement_queues() noexcept {
    retired_widgets_ = nullptr;
    collectable_widgets_ = nullptr;
}

BridgeCallbackScope::BridgeCallbackScope(
    const std::shared_ptr<BridgeCallbackState>& state) noexcept
    : state_(state) {
    if (state_) state_->enter_callback();
}

BridgeCallbackScope::~BridgeCallbackScope() {
    if (state_) state_->leave_callback();
}

std::string js_string_literal(std::string_view text) {
    return choc::json::toString(choc::value::createString(std::string(text)), false);
}

namespace {

// The one place a ScriptEngine is dereferenced on behalf of a deferred native
// callback. Every caller must have cleared its liveness guards first.
void eval_on_live_engine(ScriptEngine& engine,
                         const std::string& js,
                         const char* context) {
    try {
        if (!static_cast<bool>(engine)) return;
        engine.evaluate(js);
        // Pump microtasks so React setState commits (and any queueMicrotask /
        // Promise.then continuations scheduled by the handler) before the next
        // event arrives. Without this, drag-style interactions see stale state
        // on the immediately-following pointermove and silently bail.
        engine.pump_message_loop();
    } catch (const std::exception& e) {
        std::cerr << "WidgetBridge " << context << " error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "WidgetBridge " << context << " error: unknown exception\n";
    }
}

// `<fn>(<id>, '<event_name>', <payload_expr>)`.
std::string dispatch_call(std::string_view fn,
                          std::string_view id,
                          const std::string& event_name,
                          std::string_view payload_expr) {
    std::string js;
    js.append(fn).append("(").append(js_string_literal(id));
    js.append(", '").append(event_name).append("', ");
    js.append(payload_expr.data(), payload_expr.size());
    js += ")";
    return js;
}

}  // namespace

void safe_dispatch_eval(const std::shared_ptr<BridgeCallbackState>& alive,
                        ScriptEngine* engine,
                        const std::string& js,
                        const char* context) {
    if (!alive || !alive->load(std::memory_order_acquire) || engine == nullptr) return;
    // The bridge holds `ScriptEngine&`, so the engine is owned by the HOST, not
    // by the bridge. `alive` proves only that the bridge is still standing; a
    // host that destroys the engine first (or in the wrong order relative to
    // the bridge) leaves `engine` dangling with `alive` still true, and the
    // very next line would call a virtual through recycled storage. Ask the
    // engine's own token instead.
    if (!alive->engine_alive()) return;
    eval_on_live_engine(*engine, js, context);
}

void safe_dispatch_eval(ScriptEngine& engine, const std::string& js, const char* context) {
    // The no-flag path targets a known-valid engine reference held by a bridge
    // member function that is itself executing — nothing to outlive.
    eval_on_live_engine(engine, js, context);
}

void dispatch_event(const std::shared_ptr<BridgeCallbackState>& alive,
                    ScriptEngine* engine,
                    std::string_view id,
                    const std::string& event_name,
                    std::string_view payload_expr) {
    safe_dispatch_eval(alive, engine,
                       dispatch_call("__dispatch__", id, event_name, payload_expr),
                       event_name.c_str());
}

void dispatch_event(ScriptEngine& engine,
                    std::string_view id,
                    const std::string& event_name,
                    std::string_view payload_expr) {
    safe_dispatch_eval(engine,
                       dispatch_call("__dispatch__", id, event_name, payload_expr),
                       event_name.c_str());
}

void dispatch_callback_only(const std::shared_ptr<BridgeCallbackState>& alive,
                            ScriptEngine* engine,
                            std::string_view id,
                            const std::string& event_name,
                            std::string_view payload_expr) {
    safe_dispatch_eval(
        alive, engine,
        dispatch_call("__dispatchCallbackOnly__", id, event_name, payload_expr),
        event_name.c_str());
}

} // namespace pulp::view
