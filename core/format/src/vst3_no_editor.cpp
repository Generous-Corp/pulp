// Editor-less VST3 plug-ins (pulp_add_plugin NO_EDITOR) compile this instead of
// vst3_plug_view.cpp. The shared adapter's createView() calls make_plug_view()
// only for a processor that reports has_editor(); this definition answers with
// no view, so the module links without the view layer and a host sees a
// plug-in that has no editor.
#include <pulp/format/vst3_adapter.hpp>

namespace pulp::format::vst3 {

Steinberg::IPlugView* make_plug_view(Processor&, state::StateStore&, runtime::AliveToken::Handle) {
    return nullptr;
}

} // namespace pulp::format::vst3
