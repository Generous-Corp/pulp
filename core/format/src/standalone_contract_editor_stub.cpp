#include <pulp/format/detail/standalone_contract_editor.hpp>

#include <pulp/runtime/log.hpp>

namespace pulp::format::detail {

bool standalone_contract_editor_supported() {
    return false;
}

bool run_standalone_contract_editor(Processor&, state::StateStore&, EditorFactory&,
                                    const StandaloneContractEditorOptions&) {
    runtime::log_error("Standalone: this platform cannot host an editor-host contract editor yet");
    return false;
}

} // namespace pulp::format::detail
