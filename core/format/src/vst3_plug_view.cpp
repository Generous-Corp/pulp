// VST3 IPlugView implementation
// Creates AutoUi editor view and embeds it in the DAW's window via PluginViewHost.
// Platform-specific rendering is handled by PluginViewHost (CoreGraphics on macOS).

#ifdef PULP_VST3_GUI

#include <pulp/format/vst3_plug_view.hpp>
#include <pulp/runtime/log.hpp>
#include <cstring>

namespace pulp::format::vst3 {

using namespace Steinberg;

PulpPlugView::PulpPlugView(Processor& processor, state::StateStore& store)
    : CPluginView(nullptr)
    , processor_(processor)
    , store_(store)
{
    auto [w, h] = processor_.editor_size();
    ViewRect r(0, 0, static_cast<int32>(w), static_cast<int32>(h));
    setRect(r);
}

tresult PLUGIN_API PulpPlugView::isPlatformTypeSupported(FIDString type) {
#ifdef __APPLE__
    if (strcmp(type, kPlatformTypeNSView) == 0) return kResultTrue;
#elif defined(_WIN32)
    if (strcmp(type, kPlatformTypeHWND) == 0) return kResultTrue;
#elif defined(__linux__)
    if (strcmp(type, kPlatformTypeX11EmbedWindowID) == 0) return kResultTrue;
#endif
    return kResultFalse;
}

tresult PLUGIN_API PulpPlugView::attached(void* parent, FIDString type) {
    // Build the AutoUi view tree from parameters
    editor_root_ = view::AutoUi::build(store_);
    if (!editor_root_) {
        runtime::log_error("VST3 editor: AutoUi::build() returned nullptr");
        return kResultFalse;
    }

    auto [w, h] = processor_.editor_size();
    view::PluginViewHost::Options opts;
    opts.size = {w, h};
    opts.use_gpu = false;  // CoreGraphics for now; GPU opt-in later

    editor_host_ = view::PluginViewHost::create(*editor_root_, opts);
    if (!editor_host_) {
        runtime::log_error("VST3 editor: PluginViewHost::create() failed");
        editor_root_.reset();
        return kResultFalse;
    }

    // Attach to the host's parent window
    editor_host_->attach_to_parent(parent);

    // Let CPluginView track the system window
    auto result = CPluginView::attached(parent, type);

    runtime::log_info("VST3 editor: attached ({}x{})", w, h);
    return result;
}

tresult PLUGIN_API PulpPlugView::removed() {
    if (editor_host_) {
        editor_host_->detach();
        editor_host_.reset();
    }
    editor_root_.reset();

    runtime::log_info("VST3 editor: removed");
    return CPluginView::removed();
}

tresult PLUGIN_API PulpPlugView::getSize(ViewRect* size) {
    if (!size) return kInvalidArgument;
    auto [w, h] = processor_.editor_size();
    size->left = 0;
    size->top = 0;
    size->right = static_cast<int32>(w);
    size->bottom = static_cast<int32>(h);
    return kResultTrue;
}

tresult PLUGIN_API PulpPlugView::onSize(ViewRect* newSize) {
    if (!newSize) return kInvalidArgument;

    auto result = CPluginView::onSize(newSize);

    if (editor_host_) {
        uint32_t w = static_cast<uint32_t>(newSize->getWidth());
        uint32_t h = static_cast<uint32_t>(newSize->getHeight());
        editor_host_->set_size(w, h);
    }

    return result;
}

} // namespace pulp::format::vst3

#endif // PULP_VST3_GUI
