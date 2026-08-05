// runtime_eval_component.hpp — optional live scripted-realm evaluator adapter
#pragma once

#include <pulp/inspect/runtime_evaluator.hpp>

#include <memory>

namespace pulp::view { class ScriptInspectorBridge; }

namespace pulp::inspect {

/// Construct the separately linked high-risk adapter for a live scripted UI.
std::unique_ptr<RuntimeEvaluator>
make_script_runtime_evaluator(view::ScriptInspectorBridge* bridge);

} // namespace pulp::inspect
