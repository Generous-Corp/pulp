#pragma once

#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

// Shared by every callback installed on bridge-owned views. A JS handler may
// synchronously remove the native view whose std::function is currently
// executing. Logical removal happens immediately, while this state retains the
// detached ownership until the outermost native callback returns.
struct BridgeCallbackState {
    BridgeCallbackState(std::vector<std::unique_ptr<View>>* retired_widgets,
                        std::vector<std::unique_ptr<View>>* collectable_widgets) noexcept
        : retired_widgets_(retired_widgets),
          collectable_widgets_(collectable_widgets) {}

    std::atomic<bool> alive{true};

    bool load(std::memory_order order) const noexcept {
        return alive.load(order);
    }
    void store(bool value, std::memory_order order) noexcept {
        alive.store(value, order);
    }

    void enter_callback() noexcept;
    void leave_callback() noexcept;
    void retire(std::unique_ptr<View> widget);
    void detach_retirement_queues() noexcept;

    // Start observing the engine every callback guarded by this state will
    // dereference. `alive` alone tracks only the BRIDGE: the bridge holds
    // `ScriptEngine&` — the engine is owned OUTSIDE it — so a host that
    // destroys the engine first leaves `alive` true while every captured
    // `ScriptEngine*` dangles. Called once from the bridge constructor.
    void track_engine(std::weak_ptr<const void> token) noexcept {
        engine_token_ = std::move(token);
        engine_tracked_ = true;
    }
    // False only when a tracked engine has been destroyed. An untracked state
    // (the always-alive state backing the known-valid-reference overloads)
    // always reports true — it has no engine to outlive.
    bool engine_alive() const noexcept {
        return !engine_tracked_ || !engine_token_.expired();
    }

private:
    std::size_t callback_depth_ = 0;
    std::vector<std::unique_ptr<View>>* retired_widgets_ = nullptr;
    // The outermost callback cannot destroy its own closure safely from a
    // local scope destructor: that destructor still runs inside
    // std::function::operator(). Move completed batches here and collect them
    // at the start of the next outer callback, after the prior one returned.
    std::vector<std::unique_ptr<View>>* collectable_widgets_ = nullptr;
    std::weak_ptr<const void> engine_token_;
    bool engine_tracked_ = false;
};

class BridgeCallbackScope {
public:
    explicit BridgeCallbackScope(
        const std::shared_ptr<BridgeCallbackState>& state) noexcept;
    ~BridgeCallbackScope();

    BridgeCallbackScope(const BridgeCallbackScope&) = delete;
    BridgeCallbackScope& operator=(const BridgeCallbackScope&) = delete;

private:
    std::shared_ptr<BridgeCallbackState> state_;
};

// A JSON-style JS string literal (double-quoted, fully escaped) for `text`,
// safe to splice into evaluated JS wherever a string argument is expected.
std::string js_string_literal(std::string_view text);

// Evaluate `js` on the engine, then pump the microtask loop so React setState
// (and any queued Promise/microtask continuations) commit before the next event
// arrives. Exceptions are swallowed and logged to stderr tagged with `context`.
//
// The alive-flag overload is a no-op when the bridge has been torn down (the
// alive flag is cleared), when the ENGINE the flag tracks has been destroyed
// (see BridgeCallbackState::track_engine — the bridge does not own the engine,
// so bridge liveness does not imply engine liveness), or when the engine is
// null/invalid; the no-flag overload always runs and targets a known-valid
// engine reference.
void safe_dispatch_eval(const std::shared_ptr<BridgeCallbackState>& alive,
                        ScriptEngine* engine,
                        const std::string& js,
                        const char* context);
void safe_dispatch_eval(ScriptEngine& engine, const std::string& js, const char* context);

// Dispatch a DOM-style event into the JS bridge, i.e.
// `__dispatch__(<id>, '<event_name>', <payload_expr>)`. The target `id` is
// ALWAYS routed through js_string_literal, so an id containing a quote or
// backslash cannot break out of the string literal. `event_name` is a fixed
// internal token spliced verbatim; `payload_expr` is a ready-to-eval JS
// expression. The event name doubles as the stderr error context.
void dispatch_event(const std::shared_ptr<BridgeCallbackState>& alive,
                    ScriptEngine* engine,
                    std::string_view id,
                    const std::string& event_name,
                    std::string_view payload_expr);
void dispatch_event(ScriptEngine& engine,
                    std::string_view id,
                    const std::string& event_name,
                    std::string_view payload_expr);

// Dispatch only the low-level `on(id, event_name, fn)` callback. Unlike
// dispatch_event(), this does not fan out through Element::dispatchEvent and is
// used for native ancestors above a DOM event's single dispatch origin.
void dispatch_callback_only(const std::shared_ptr<BridgeCallbackState>& alive,
                            ScriptEngine* engine,
                            std::string_view id,
                            const std::string& event_name,
                            std::string_view payload_expr);

} // namespace pulp::view
