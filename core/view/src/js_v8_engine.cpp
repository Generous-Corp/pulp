// V8 backend for the Pulp JS engine abstraction.
// Gated behind PULP_HAS_V8 — requires V8 headers and libraries on the include/link path.
// V8 is BSD-3-Clause licensed.
//
// Build integration:
//   cmake -S . -B build -DPULP_JS_ENGINE=v8 \
//         -DV8_INCLUDE_DIR=/path/to/v8/include \
//         -DV8_LIB_DIR=/path/to/v8/lib \
//         [-DV8_LIBRARY_PATH=/full/path/to/libv8_monolith_or_libnode]
//
// V8 provides JIT compilation, making it suitable for heavy workloads like
// Three.js scene graphs. QuickJS is the portable fallback.

#include <pulp/view/js_engine.hpp>

#ifdef PULP_HAS_V8

// CHOC's V8 + Console headers use std::ostringstream / std::endl / std::cerr /
// std::cout but rely on a transitive include of <sstream>/<iostream>. The MSVC
// STL pulls those in implicitly; libc++ (the Chromium-style libc++ the Windows
// sealed v8.dll requires its consumer to use — see tools/cmake/PulpV8Windows.
// cmake) does NOT, so the iostream headers must be included explicitly BEFORE
// the choc headers or the TU fails to compile under clang-cl + libc++.
#include <ios>
#include <ostream>
#include <sstream>
#include <iostream>

// Include CHOC's V8 wrapper — must appear in exactly one translation unit
#include <choc/javascript/choc_javascript_V8.h>
#include <choc/javascript/choc_javascript_Console.h>

#include <stdexcept>

namespace pulp::view {

static std::string_view v8_logging_level_name(choc::javascript::LoggingLevel level) {
    switch (level) {
        case choc::javascript::LoggingLevel::log:   return "log";
        case choc::javascript::LoggingLevel::info:  return "info";
        case choc::javascript::LoggingLevel::warn:  return "warn";
        case choc::javascript::LoggingLevel::error: return "error";
        case choc::javascript::LoggingLevel::debug: return "debug";
        default: return "log";
    }
}

class V8Engine final : public JsEngine {
public:
    V8Engine()
        : context_(choc::javascript::createV8Context())
    {
        setup_console();
    }

    JsEngineType type() const override { return JsEngineType::v8; }

    std::string runtime_version() const override {
        // The version V8 itself reports at runtime — the ground truth that the
        // sealed provider claim (PULP_V8_EXPECTED_RUNTIME_VERSION) is checked
        // against. v8::V8::GetVersion() is declared in v8-initialization.h,
        // transitively included via choc_javascript_V8.h.
        const char* version = v8::V8::GetVersion();
        return version ? std::string(version) : std::string{};
    }

    std::string provider_kind() const override {
#ifdef PULP_V8_PROVIDER_KIND
        return PULP_V8_PROVIDER_KIND;
#else
        return {};
#endif
    }

    std::string provider_path() const override {
#ifdef PULP_V8_PROVIDER_PATH
        return PULP_V8_PROVIDER_PATH;
#else
        return {};
#endif
    }

    std::string expected_runtime_version() const override {
#ifdef PULP_V8_EXPECTED_RUNTIME_VERSION
        return PULP_V8_EXPECTED_RUNTIME_VERSION;
#else
        return {};
#endif
    }

    bool is_valid() const override { return static_cast<bool>(context_); }

    choc::value::Value evaluate(const std::string& code) override {
        return context_.evaluateExpression(code);
    }

    void run_module(const std::string& code,
                    ModuleResolver resolver,
                    ModuleCompletionHandler completion) override {
        context_.runModule(code,
            [resolver = std::move(resolver)](std::string_view path) -> std::optional<std::string> {
                return resolver(path);
            },
            [completion = std::move(completion)](const std::string& error,
                                                 const choc::value::ValueView& result) mutable {
                if (completion) {
                    completion(error, choc::value::Value(result));
                }
            });
    }

    void register_function_impl(const std::string& name, NativeFunction fn) override {
        context_.registerFunction(name,
            [fn = std::move(fn)](choc::javascript::ArgumentList args) -> choc::value::Value {
                return fn(args.args, args.numArgs);
            });
    }

    choc::value::Value invoke(std::string_view name) override {
        return context_.invoke(name);
    }

    choc::value::Value invoke(std::string_view name,
                              const choc::value::Value* args,
                              size_t num_args) override {
        switch (num_args) {
            case 0: return context_.invoke(name);
            case 1: return context_.invoke(name, args[0]);
            case 2: return context_.invoke(name, args[0], args[1]);
            case 3: return context_.invoke(name, args[0], args[1], args[2]);
            case 4: return context_.invoke(name, args[0], args[1], args[2], args[3]);
            default: {
                std::string call(name);
                call += '(';
                for (size_t i = 0; i < num_args; ++i) {
                    if (i > 0) call += ',';
                    call += choc::json::toString(args[i]);
                }
                call += ')';
                return context_.evaluateExpression(call);
            }
        }
    }

    void set_log_callback(LogCallback callback) override {
        log_callback_ = std::move(callback);
        setup_console();
    }

    // V8 has excellent GC but we can request a low-priority collection
    void gc_hint() override {}

    void pump_message_loop() override {
        context_.pumpMessageLoop();
    }

    // Host objects are wired through the shared engine-agnostic native-backed
    // object descriptor seam.
    bool supports_host_objects() const override { return true; }
    bool supports_typed_arrays() const override { return true; }
    bool supports_promises() const override { return true; }

private:
    choc::javascript::Context context_;
    LogCallback log_callback_;

    void setup_console() {
        choc::javascript::registerConsoleFunctions(context_,
            [this](std::string_view message, choc::javascript::LoggingLevel level) {
                if (log_callback_)
                    log_callback_(v8_logging_level_name(level), message);
            });
    }
};

std::unique_ptr<JsEngine> create_v8_engine() {
    return std::make_unique<V8Engine>();
}

} // namespace pulp::view

#else // !PULP_HAS_V8

namespace pulp::view {

std::unique_ptr<JsEngine> create_v8_engine() {
    throw std::runtime_error("V8 engine not available — build with PULP_JS_ENGINE=v8 and provide V8 libraries");
}

} // namespace pulp::view

#endif // PULP_HAS_V8
