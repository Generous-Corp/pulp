#pragma once

#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/view_lifecycle.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

// Shared by every callback installed on bridge-owned views. A JS handler may
// synchronously remove the native view whose std::function is currently
// executing. Logical removal happens immediately, while the detached ownership
// is retained until the outermost native callback returns.
//
// That retention now lives on the tree ROOT (View::retire, view_lifecycle.hpp)
// instead of in bridge-owned vectors, so a view removed by a JS handler and one
// removed by a native lifecycle hook obey a single contract with a single
// owner. The root pointer rides here because most callback closures capture
// only `alive` — they have neither the bridge nor the root in scope — so this
// is the one object every call site already holds.
struct BridgeCallbackState {
    explicit BridgeCallbackState(View* dispatch_root) noexcept
        : dispatch_root_(dispatch_root) {}

    std::atomic<bool> alive{true};

    bool load(std::memory_order order) const noexcept {
        return alive.load(order);
    }
    void store(bool value, std::memory_order order) noexcept {
        alive.store(value, order);
    }

    /// The tree whose mutation gate a callback scope raises. Null for the
    /// always-alive sentinels and after the owning bridge is retired.
    View* dispatch_root() const noexcept { return dispatch_root_; }
    void retire_dispatch() noexcept;

    void track_engine(std::weak_ptr<const void> token) noexcept {
        engine_token_ = std::move(token);
        engine_tracked_ = true;
    }
    bool engine_alive() const noexcept {
        return !engine_tracked_ || !engine_token_.expired();
    }

    std::recursive_mutex& dispatch_mutex() noexcept { return dispatch_mutex_; }

private:
    // Cleared by retire_dispatch() so a callback that fires after its bridge is
    // retired raises no gate on a tree the bridge no longer serves.
    View* dispatch_root_ = nullptr;
    // Native input may race an owner-driven realm teardown. Serialize the
    // alive check and engine evaluation with retirement so no callback can
    // dereference the borrowed ScriptEngine after its bridge is retired.
    std::recursive_mutex dispatch_mutex_;
    std::weak_ptr<const void> engine_token_;
    bool engine_tracked_ = false;
};

// Raises the tree's mutation gate for the duration of one native callback, so
// a JS handler that removes the view it is running on cannot free that view
// before the callback returns.
//
// The lease is `deferred`: this scope is constructed as the first statement of
// the callback's own `std::function` body, so its destructor runs while
// `std::function::operator()` is still on the stack. Draining there could
// destroy the closure that is executing; the retired views are instead freed
// when the next outermost lease opens.
class BridgeCallbackScope {
public:
    explicit BridgeCallbackScope(
        const std::shared_ptr<BridgeCallbackState>& state) noexcept;
    ~BridgeCallbackScope();

    BridgeCallbackScope(const BridgeCallbackScope&) = delete;
    BridgeCallbackScope& operator=(const BridgeCallbackScope&) = delete;

private:
    std::shared_ptr<BridgeCallbackState> state_;
    std::optional<DispatchLease> lease_;
};

// A JSON-style JS string literal (double-quoted, fully escaped) for `text`,
// safe to splice into evaluated JS wherever a string argument is expected.
std::string js_string_literal(std::string_view text);

// Evaluate `js` on the engine, then pump the microtask loop so React setState
// (and any queued Promise/microtask continuations) commit before the next event
// arrives. Exceptions are swallowed and logged to stderr tagged with `context`.
//
// The alive-flag overload is a no-op when the bridge has been torn down (the
// alive flag is cleared) or the engine is null/invalid; the no-flag overload
// always runs and targets a known-valid engine reference.
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
