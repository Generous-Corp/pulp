// QuickJS backend for the Pulp JS engine abstraction.
// Wraps choc::javascript::Context (QuickJS) — the original and portable default.

// Include QuickJS implementation — must appear in exactly one translation unit
#include <choc/javascript/choc_javascript_QuickJS.h>
#include <choc/javascript/choc_javascript_Console.h>

#include <pulp/view/js_engine.hpp>
#include <stdexcept>

namespace pulp::view {

// ── QuickJS stack size increase ─────────────────────────────────────────────
// The web-compat prelude + deep JS↔C++ interleaving during DOM operations
// (appendChild → _reparentNative → native bridge → back to JS) exhausts
// QuickJS's default 256KB JS stack. We increase it to 1MB.
//
// CHOC's Context::pimpl is private, but we need the JSRuntime* to call
// JS_SetMaxStackSize. Since this file includes the full QuickJS implementation,
// we have access to quickjs::QuickJSContext (which has a public JSRuntime*
// runtime member). We access it through the Context's known single-member
// layout: { unique_ptr<Pimpl> pimpl }.

static void set_quickjs_stack_size(choc::javascript::Context& ctx, size_t size) {
    struct ContextLayout {
        std::unique_ptr<choc::javascript::Context::Pimpl> pimpl;
    };
    auto& layout = reinterpret_cast<ContextLayout&>(ctx);
    auto* qjctx = static_cast<choc::javascript::quickjs::QuickJSContext*>(layout.pimpl.get());
    if (qjctx && qjctx->runtime)
        JS_SetMaxStackSize(qjctx->runtime, size);
}

// CHOC's QuickJS backend declares pumpMessageLoop() with an empty body
// (see external choc_javascript_QuickJS.h). That makes
// queueMicrotask / Promise.then / async-await never drain when callers
// invoke pump_message_loop() — see pulp #746. We work around it the
// same way set_quickjs_stack_size does: reach into the pimpl, grab the
// JSContext*, and call JS_ExecutePendingJob until the queue is empty.
// JS_ExecutePendingJob returns 1 when it ran a job, 0 when the queue is
// empty, and a negative value on a JS exception inside the job — we
// stop on either terminal state.
//
// Per Codex P2 on PR #769: the previous implementation capped at 4096
// jobs and returned silently, which meant a Promise/microtask chain
// larger than the cap (rare but possible with bundled frameworks)
// would be silently half-drained. Browsers and Node don't cap; the JS
// spec says microtasks drain to completion before returning to the
// macrotask loop. A genuine runaway microtask self-loop is a JS bug
// the test will surface as a timeout, which is correct. Drain to empty
// and trust callers to fix any test that legitimately can't terminate.
static void pump_quickjs_jobs(choc::javascript::Context& ctx) {
    struct ContextLayout {
        std::unique_ptr<choc::javascript::Context::Pimpl> pimpl;
    };
    auto& layout = reinterpret_cast<ContextLayout&>(ctx);
    auto* qjctx = static_cast<choc::javascript::quickjs::QuickJSContext*>(layout.pimpl.get());
    if (!qjctx || !qjctx->runtime) return;
    choc::javascript::quickjs::JSContext* pctx = nullptr;
    for (;;) {
        int rc = JS_ExecutePendingJob(qjctx->runtime, &pctx);
        if (rc <= 0) return;  // 0 = empty queue, <0 = JS exception inside job
    }
}

static std::string_view logging_level_name(choc::javascript::LoggingLevel level) {
    switch (level) {
        case choc::javascript::LoggingLevel::log:   return "log";
        case choc::javascript::LoggingLevel::info:  return "info";
        case choc::javascript::LoggingLevel::warn:  return "warn";
        case choc::javascript::LoggingLevel::error: return "error";
        case choc::javascript::LoggingLevel::debug: return "debug";
        default: return "log";
    }
}

class QuickJsEngine final : public JsEngine {
public:
    QuickJsEngine()
        : context_(choc::javascript::createQuickJSContext())
    {
        set_quickjs_stack_size(context_, 1024 * 1024);  // 1MB (up from 256KB)
        setup_console();
    }

    JsEngineType type() const override { return JsEngineType::quickjs; }
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

    void register_function(const std::string& name, NativeFunction fn) override {
        // Adapt Pulp's NativeFunction to CHOC's NativeFunction signature
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
        // CHOC's invoke is variadic template — we need to dispatch by arg count
        switch (num_args) {
            case 0: return context_.invoke(name);
            case 1: return context_.invoke(name, args[0]);
            case 2: return context_.invoke(name, args[0], args[1]);
            case 3: return context_.invoke(name, args[0], args[1], args[2]);
            case 4: return context_.invoke(name, args[0], args[1], args[2], args[3]);
            default: {
                // For 5+ args, build a JS call expression
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

    void gc_hint() override {
        // QuickJS runs GC automatically; we can trigger a cycle via evaluate
        // but it's not necessary — the runtime handles it well.
    }

    void pump_message_loop() override {
        // CHOC's pumpMessageLoop is a no-op for QuickJS (#746); drive the
        // pending-job queue ourselves so queueMicrotask / Promise.then /
        // async-await actually drain when callers ask.
        pump_quickjs_jobs(context_);
    }

    bool supports_host_objects() const override { return true; }
    bool supports_promises() const override { return true; }

    // Expose the underlying CHOC context for WidgetBridge backward compatibility
    choc::javascript::Context& choc_context() { return context_; }

private:
    choc::javascript::Context context_;
    LogCallback log_callback_;

    void setup_console() {
        choc::javascript::registerConsoleFunctions(context_,
            [this](std::string_view message, choc::javascript::LoggingLevel level) {
                if (log_callback_)
                    log_callback_(logging_level_name(level), message);
            });
    }
};

std::unique_ptr<JsEngine> create_quickjs_engine() {
    return std::make_unique<QuickJsEngine>();
}

} // namespace pulp::view
