#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <optional>
#include <memory>
#include <vector>
#include <cstdint>
#include <choc/containers/choc_Value.h>

namespace pulp::view {

// Callback for resolving module imports (returns source code or nullopt)
using ModuleResolver = std::function<std::optional<std::string>(std::string_view path)>;

// Callback for logging from JS (console.log, console.warn, etc.)
using LogCallback = std::function<void(std::string_view level, std::string_view message)>;

// Native function callable from JS
using NativeFunction = std::function<choc::value::Value(const choc::value::Value* args, size_t num_args)>;

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

    // Run JS code as a module with import resolution
    virtual void run_module(const std::string& code, ModuleResolver resolver) = 0;

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

    // Hint that now is a good time to collect garbage (advisory)
    virtual void gc_hint() {}

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

// Create a JS engine using the platform default:
//   Apple → JSC, others → QuickJS (V8 if PULP_HAS_V8)
std::unique_ptr<JsEngine> create_default_js_engine();

// Query which engines are available in this build
bool is_engine_available(JsEngineType type);

} // namespace pulp::view
