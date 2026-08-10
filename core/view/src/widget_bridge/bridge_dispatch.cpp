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

void safe_dispatch_eval(const std::shared_ptr<BridgeCallbackState>& alive,
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
    static const auto always_alive =
        std::make_shared<BridgeCallbackState>(nullptr, nullptr);
    safe_dispatch_eval(always_alive, &engine, js, context);
}

void dispatch_event(const std::shared_ptr<BridgeCallbackState>& alive,
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
    static const auto always_alive =
        std::make_shared<BridgeCallbackState>(nullptr, nullptr);
    dispatch_event(always_alive, &engine, id, event_name, payload_expr);
}

void dispatch_callback_only(const std::shared_ptr<BridgeCallbackState>& alive,
                            ScriptEngine* engine,
                            std::string_view id,
                            const std::string& event_name,
                            std::string_view payload_expr) {
    std::string js = "__dispatchCallbackOnly__(" + js_string_literal(id) +
                     ", '" + event_name + "', ";
    js.append(payload_expr.data(), payload_expr.size());
    js += ")";
    safe_dispatch_eval(alive, engine, js, event_name.c_str());
}

} // namespace pulp::view
