// The EditorBridge attachment points that need a script engine or a scripted
// session. The dispatch core (handlers, dispatch, response helpers) is
// editor_bridge.cpp in the view-free pulp-editor-bridge target; these live in
// pulp-view-tooling because they name view-script types.

#include "pulp/view/editor_bridge_attach.hpp"

#include "pulp/view/script_engine.hpp"
#include "pulp/view/scripted_ui.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace pulp::view {

void attach_native_runtime(EditorBridge& /*bridge*/, JsRuntime& /*runtime*/,
                           std::string_view /*handler_name*/) {
    // Intentionally empty: the runtime-import lane needs a postMessage-style
    // primitive on JsRuntime that calls back into C++. The overload exists so
    // that lane can attach without a parallel dispatch model.
}

void attach_native_runtime(EditorBridge& bridge, ScriptEngine& engine,
                           std::string_view handler_name) {
    if (handler_name.empty())
        throw std::invalid_argument("native editor bridge handler name must not be empty");

    engine.register_function(
        std::string(handler_name), [&bridge](const choc::value::Value* args, std::size_t num_args) {
            if (num_args != 1 || args == nullptr || !args[0].isString()) {
                return choc::value::Value(
                    EditorBridge::err_response("native editor bridge expects one JSON string"));
            }
            return choc::value::Value(bridge.dispatch_json(args[0].getString()));
        });
}

void attach_native_runtime(EditorBridge& bridge, ScriptedUiSession& session,
                           std::string_view handler_name) {
    if (handler_name.empty())
        throw std::invalid_argument("native editor bridge handler name must not be empty");

    session.attach_native_message_handler(
        std::string(handler_name),
        [&bridge](std::string_view envelope_json) { return bridge.dispatch_json(envelope_json); });
}

void detach_native_runtime(EditorBridge& /*bridge*/, ScriptedUiSession& session,
                           std::string_view handler_name) {
    session.detach_native_message_handler(handler_name);
}

} // namespace pulp::view
