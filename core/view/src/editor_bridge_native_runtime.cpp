// The EditorBridge attachment points that need a script engine or a scripted
// session. The dispatch core (handlers, dispatch, response helpers) is
// editor_bridge.cpp in the view-free pulp-editor-bridge target; these live in
// pulp-view-tooling because they name view-script types.

#include "pulp/view/editor_bridge.hpp"

#include "pulp/view/script_engine.hpp"
#include "pulp/view/scripted_ui.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace pulp::view {

void EditorBridge::attach_native_runtime(JsRuntime& /*runtime*/,
                                         std::string_view /*handler_name*/) {
    // Stub for pulp #468 (Claude Design import lane). The full wiring
    // lands when JsRuntime exposes a postMessage-equivalent primitive
    // that calls back into C++. The interface is defined here so #468
    // plugs in without designing a parallel dispatch model.
}

void EditorBridge::attach_native_runtime(ScriptEngine& engine, std::string_view handler_name) {
    if (handler_name.empty())
        throw std::invalid_argument("native editor bridge handler name must not be empty");

    engine.register_function(std::string(handler_name), [this](const choc::value::Value* args,
                                                               std::size_t num_args) {
        if (num_args != 1 || args == nullptr || !args[0].isString()) {
            return choc::value::Value(err_response("native editor bridge expects one JSON string"));
        }
        return choc::value::Value(dispatch_json(args[0].getString()));
    });
}

void EditorBridge::attach_native_runtime(ScriptedUiSession& session,
                                         std::string_view handler_name) {
    if (handler_name.empty())
        throw std::invalid_argument("native editor bridge handler name must not be empty");

    session.attach_native_message_handler(
        std::string(handler_name),
        [this](std::string_view envelope_json) { return dispatch_json(envelope_json); });
}

void EditorBridge::detach_native_runtime(ScriptedUiSession& session,
                                         std::string_view handler_name) {
    session.detach_native_message_handler(handler_name);
}

} // namespace pulp::view
