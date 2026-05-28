#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <optional>
#include <memory>
#include <vector>
#include <cstdint>
#include <choc/containers/choc_Value.h>
#include <choc/text/choc_JSON.h>

namespace pulp::view {

// Callback for resolving module imports (returns source code or nullopt)
using ModuleResolver = std::function<std::optional<std::string>(std::string_view path)>;
using ModuleCompletionHandler = std::function<void(const std::string& error, const choc::value::Value& result)>;

// Callback for logging from JS (console.log, console.warn, etc.)
using LogCallback = std::function<void(std::string_view level, std::string_view message)>;

// Native function callable from JS
using NativeFunction = std::function<choc::value::Value(const choc::value::Value* args, size_t num_args)>;
using NativePromiseFunction = NativeFunction;

// First host-object slice: native-backed global objects with snapshot properties
// and native method callbacks. This is intentionally smaller than a full opaque
// proxy / HostObject API, but it gives later bridge phases a truthful native
// object seam to build on.
struct HostObjectProperty {
    std::string name;
    choc::value::Value value;
};

struct HostObjectMethod {
    std::string name;
    NativeFunction fn;
};

struct HostObjectDescriptor {
    std::string class_name;
    std::vector<HostObjectProperty> properties;
    std::vector<HostObjectMethod> methods;
};

struct JsEngineCreationStats {
    uint64_t total = 0;
    uint64_t quickjs = 0;
    uint64_t jsc = 0;
    uint64_t v8 = 0;
};

// Which JS engine backend is active
enum class JsEngineType {
    quickjs,
    jsc,
    v8
};

// Returns a human-readable name for the engine type
constexpr std::string_view engine_type_name(JsEngineType type) {
    switch (type) {
        case JsEngineType::quickjs: return "QuickJS";
        case JsEngineType::jsc:    return "JavaScriptCore";
        case JsEngineType::v8:     return "V8";
    }
    return "unknown";
}

// Abstract JS engine backend.
// Each backend implements this interface to provide a uniform scripting API.
// Not thread-safe — all calls must happen on the same thread.
class JsEngine {
public:
    virtual ~JsEngine() = default;

    // Which engine type this is
    virtual JsEngineType type() const = 0;

    // Check if the engine is in a valid state
    virtual bool is_valid() const = 0;

    // Evaluate JS code synchronously, returns the result.
    // Throws std::runtime_error on parse/runtime errors.
    virtual choc::value::Value evaluate(const std::string& code) = 0;

    // Run JS code as a module with import resolution. Completion is invoked
    // when the engine reports module execution has either succeeded or failed.
    virtual void run_module(const std::string& code,
                            ModuleResolver resolver,
                            ModuleCompletionHandler completion = {}) = 0;

    // Register a C++ function callable from JS as a global
    virtual void register_function(const std::string& name, NativeFunction fn) = 0;

    // Invoke a global JS function by name with no arguments
    virtual choc::value::Value invoke(std::string_view name) = 0;

    // Invoke a global JS function with positional arguments
    virtual choc::value::Value invoke(std::string_view name,
                                      const choc::value::Value* args,
                                      size_t num_args) = 0;

    // Set a log callback for console.log/warn/error/info/debug
    virtual void set_log_callback(LogCallback callback) = 0;

    // Register a native-backed global object with snapshot properties and
    // native method callbacks.
    virtual void register_host_object(const std::string& name, HostObjectDescriptor descriptor) {
        const auto quote_string = [] (std::string_view text) {
            return choc::json::toString(choc::value::createString(std::string(text)));
        };

        const auto js_literal = [&] (const choc::value::Value& value) -> std::string {
            if (value.isVoid())
                return "undefined";
            return choc::json::toString(value);
        };

        static uint64_t global_host_symbol = 0;

        std::string target = "globalThis[" + quote_string(name) + "]";
        std::string script;
        script.reserve(256 + descriptor.properties.size() * 64 + descriptor.methods.size() * 96);
        script += target + " = {};\n";

        if (!descriptor.class_name.empty())
            script += target + "[\"_objectName\"] = " + quote_string(descriptor.class_name) + ";\n";

        for (const auto& property : descriptor.properties)
            script += target + "[" + quote_string(property.name) + "] = " + js_literal(property.value) + ";\n";

        for (auto& method : descriptor.methods) {
            auto hidden_name = "__pulp_host_object_" + std::to_string(global_host_symbol++);
            register_function(hidden_name, std::move(method.fn));
            script += target + "[" + quote_string(method.name) + "] = globalThis[" + quote_string(hidden_name) + "];\n";
            script += "delete globalThis[" + quote_string(hidden_name) + "];\n";
        }

        evaluate(script + target + ";");
    }

    // First promise slice: expose a native callback as a JS function that
    // returns a real Promise and resolves on the JS microtask queue.
    // This does not yet provide a held native resolver for later completion.
    virtual void register_promise_function(const std::string& name, NativePromiseFunction fn) {
        const auto quote_string = [] (std::string_view text) {
            return choc::json::toString(choc::value::createString(std::string(text)));
        };

        static uint64_t global_promise_symbol = 0;
        auto hidden_name = "__pulp_promise_function_" + std::to_string(global_promise_symbol++);
        register_function(hidden_name, std::move(fn));

        std::string quoted_hidden = quote_string(hidden_name);
        std::string quoted_name = quote_string(name);
        std::string script =
            "globalThis[" + quoted_name + "] = (...args) => Promise.resolve().then(() => globalThis[" + quoted_hidden + "](...args));\n"
            "delete globalThis[" + quoted_hidden + "];\n"
            "globalThis[" + quoted_name + "];";
        evaluate(script);
    }

    // Hint that now is a good time to collect garbage (advisory)
    virtual void gc_hint() {}

    // Pump any engine-specific message loop / microtask queue. This is a no-op
    // for engines that do not expose an explicit pump hook.
    virtual void pump_message_loop() {}

    // ── Phase 13 forward-compatibility (HostObject / TypedArray / Promise) ──
    // These are defined now so all backends can be designed with them in mind.
    // Default implementations return false / no-op. Backends enable as ready.

    virtual bool supports_host_objects() const { return false; }
    // This means the engine can surface JS TypedArray / ArrayBuffer values
    // through the current Pulp API seam without collapsing them into an
    // opaque generic object. It does not imply zero-copy bridging yet.
    virtual bool supports_typed_arrays() const { return false; }
    virtual bool supports_promises() const { return false; }

    // Non-copyable, non-movable (owned via unique_ptr)
    JsEngine(const JsEngine&) = delete;
    JsEngine& operator=(const JsEngine&) = delete;
    JsEngine(JsEngine&&) = delete;
    JsEngine& operator=(JsEngine&&) = delete;

protected:
    JsEngine() = default;
};

// ── Engine factory ─────────────────────────────────────────────────────────

// Create a JS engine of the specified type.
// Throws std::runtime_error if the requested engine is not available on this platform/build.
std::unique_ptr<JsEngine> create_js_engine(JsEngineType type);

// Create a JS engine using the build default. QuickJS remains the
// compatibility default unless a PULP_DEFAULT_ENGINE_* option selects
// JavaScriptCore or V8 at compile time.
std::unique_ptr<JsEngine> create_default_js_engine();

// Query which engines are available in this build
bool is_engine_available(JsEngineType type);

// Diagnostic hook used by native-import validation to prove that a generated
// C++ view path did not initialize a scripting backend while building/rendering.
JsEngineCreationStats js_engine_creation_stats();
void reset_js_engine_creation_stats_for_tests();

} // namespace pulp::view
