// ScriptEngine — backward-compatible wrapper around JsEngine backends.
// Delegates to the selected backend (QuickJS, JSC, V8) while preserving
// the original API that WidgetBridge and all existing callers depend on.

#include <pulp/view/script_engine.hpp>
#include <stdexcept>

namespace pulp::view {

ScriptEngine::ScriptEngine()
    : engine_(create_default_js_engine())
{
}

ScriptEngine::ScriptEngine(JsEngineType engine_type)
    : engine_(create_js_engine(engine_type))
{
}

ScriptEngine::~ScriptEngine() = default;

ScriptEngine::ScriptEngine(ScriptEngine&&) noexcept = default;
ScriptEngine& ScriptEngine::operator=(ScriptEngine&&) noexcept = default;

choc::value::Value ScriptEngine::evaluate(const std::string& code) {
    return engine_->evaluate(code);
}

void ScriptEngine::run_module(const std::string& code, ModuleResolver resolver) {
    engine_->run_module(code, std::move(resolver));
}

void ScriptEngine::register_function(const std::string& name, NativeFunction fn) {
    engine_->register_function(name, std::move(fn));
}

void ScriptEngine::register_function(const std::string& name,
                                     choc::javascript::Context::NativeFunction fn) {
    // Adapt CHOC's ArgumentList-based signature to Pulp's NativeFunction
    engine_->register_function(name,
        [fn = std::move(fn)](const choc::value::Value* args, size_t num_args) -> choc::value::Value {
            choc::javascript::ArgumentList arg_list;
            arg_list.args = args;
            arg_list.numArgs = num_args;
            return fn(arg_list);
        });
}

void ScriptEngine::register_host_object(const std::string& name, HostObjectDescriptor descriptor) {
    engine_->register_host_object(name, std::move(descriptor));
}

void ScriptEngine::register_promise_function(const std::string& name, NativePromiseFunction fn) {
    engine_->register_promise_function(name, std::move(fn));
}

choc::value::Value ScriptEngine::invoke(std::string_view name) {
    return engine_->invoke(name);
}

void ScriptEngine::set_log_callback(LogCallback callback) {
    engine_->set_log_callback(std::move(callback));
}

ScriptEngine::operator bool() const {
    return engine_ && engine_->is_valid();
}

JsEngineType ScriptEngine::engine_type() const {
    return engine_ ? engine_->type() : JsEngineType::quickjs;
}

JsEngine& ScriptEngine::engine() {
    return *engine_;
}

const JsEngine& ScriptEngine::engine() const {
    return *engine_;
}

} // namespace pulp::view
