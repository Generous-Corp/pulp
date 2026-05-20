#pragma once

#include <pulp/view/js_engine.hpp>
#include <string>
#include <string_view>
#include <functional>
#include <optional>
#include <memory>
#include <choc/javascript/choc_javascript.h>

namespace pulp::view {

// JS scripting engine for plugin UIs.
// Wraps a JsEngine backend (QuickJS, JSC, or V8).
// Not thread-safe — all calls must happen on the same thread.
//
// This class preserves the original ScriptEngine API for backward compatibility
// with WidgetBridge and all existing callers. Internally it delegates to the
// selected JsEngine backend.
class ScriptEngine {
public:
    // Create with the build default engine.
    ScriptEngine();

    // Create with a specific engine type
    explicit ScriptEngine(JsEngineType engine_type);

    ~ScriptEngine();

    ScriptEngine(ScriptEngine&&) noexcept;
    ScriptEngine& operator=(ScriptEngine&&) noexcept;

    ScriptEngine(const ScriptEngine&) = delete;
    ScriptEngine& operator=(const ScriptEngine&) = delete;

    // Evaluate JS code synchronously, returns the result as a choc::value::Value
    // Throws on parse/runtime errors
    choc::value::Value evaluate(const std::string& code);

    // Run JS code asynchronously (for module-style code with imports)
    void run_module(const std::string& code,
                    ModuleResolver resolver,
                    ModuleCompletionHandler completion = {});

    // Register a C++ function callable from JS.
    // Accepts both the new NativeFunction signature and CHOC's ArgumentList signature.
    void register_function(const std::string& name, NativeFunction fn);
    void register_function(const std::string& name, choc::javascript::Context::NativeFunction fn);
    void register_promise_function(const std::string& name, NativePromiseFunction fn);

    // Register a native-backed global object with snapshot properties and
    // native methods.
    void register_host_object(const std::string& name, HostObjectDescriptor descriptor);

    // Invoke a global JS function by name with no arguments
    choc::value::Value invoke(std::string_view name);

    // Invoke a global JS function with arguments
    template<typename... Args>
    choc::value::Value invoke(std::string_view name, Args&&... args);

    // Set a log callback for console.log/warn/error from JS
    void set_log_callback(LogCallback callback);

    // Pump any engine-specific message loop / microtask queue.
    void pump_message_loop();

    // Check if the engine is valid
    explicit operator bool() const;

    // Query which engine backend is active
    JsEngineType engine_type() const;

    // Access the underlying engine (for advanced use / testing)
    JsEngine& engine();
    const JsEngine& engine() const;

private:
    std::unique_ptr<JsEngine> engine_;

    // For QuickJS backward compatibility: WidgetBridge uses CHOC's Context directly
    // for the stack size hack and pimpl access. We keep a reference to the CHOC
    // context when the backend is QuickJS.
    choc::javascript::Context* choc_context_ = nullptr;

    void init_choc_compat();
};

// ── Template implementations ────────────────────────────────────────────────

template<typename... Args>
choc::value::Value ScriptEngine::invoke(std::string_view name, Args&&... args) {
    // Convert variadic args to choc::value::Value array
    choc::value::Value arg_values[] = { choc::value::Value(std::forward<Args>(args))... };
    return engine_->invoke(name, arg_values, sizeof...(args));
}

} // namespace pulp::view
