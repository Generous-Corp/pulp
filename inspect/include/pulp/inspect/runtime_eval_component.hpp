// runtime_eval_component.hpp — optional live scripted-realm evaluator adapter
#pragma once

#include <pulp/inspect/runtime_evaluator.hpp>

#include <memory>
#include <functional>

namespace pulp::view {
class ScriptInspectorBridge;
class ScriptedUiSession;
}

namespace pulp::inspect {

/// Construct the separately linked high-risk adapter for a live scripted UI.
std::unique_ptr<RuntimeEvaluator>
make_script_runtime_evaluator(view::ScriptInspectorBridge* bridge);

/// Construct a reload-safe evaluator that borrows the current scripted realm
/// only for each bounded evaluator call.
using ScriptedUiSessionVisitor = std::function<void(
    const std::function<void(view::ScriptedUiSession*)>&)>;
std::unique_ptr<RuntimeEvaluator>
make_script_runtime_evaluator(ScriptedUiSessionVisitor visit_session);

} // namespace pulp::inspect
