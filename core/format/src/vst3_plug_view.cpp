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
    , bridge_(processor, store)
{
    const auto& hints = bridge_.size_hints();
    ViewRect r(0, 0, static_cast<int32>(hints.preferred_width),
               static_cast<int32>(hints.preferred_height));
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
    std::string editor_error;
    if (!bridge_.open(&editor_error)) {
        runtime::log_error("VST3 editor: failed to build editor UI ({})", editor_error);
        return kResultFalse;
    }

    const auto& hints = bridge_.size_hints();
    view::PluginViewHost::Options opts;
    opts.size = {hints.preferred_width, hints.preferred_height};
    opts.use_gpu = false;  // CoreGraphics for now; GPU opt-in later

    editor_host_ = view::PluginViewHost::create(*bridge_.view(), opts);
    if (!editor_host_) {
        runtime::log_error("VST3 editor: PluginViewHost::create() failed");
        bridge_.close();
        return kResultFalse;
    }

    editor_host_->attach_to_parent(parent);
    auto result = CPluginView::attached(parent, type);
    if (result != kResultTrue) {
        editor_host_->detach();
        editor_host_.reset();
        bridge_.close();
        return result;
    }

    // Attach succeeded — now fire Processor::on_view_opened.
    bridge_.notify_attached();

    runtime::log_info("VST3 editor: attached ({}x{}, mode={})",
                      hints.preferred_width, hints.preferred_height,
                      bridge_.uses_script_ui() ? "scripted" : "autoui");
    return result;
}

tresult PLUGIN_API PulpPlugView::removed() {
    if (editor_host_) {
        editor_host_->detach();
        editor_host_.reset();
    }
    bridge_.close();

    runtime::log_info("VST3 editor: removed");
    return CPluginView::removed();
}

tresult PLUGIN_API PulpPlugView::getSize(ViewRect* size) {
    if (!size) return kInvalidArgument;
    const auto& hints = bridge_.size_hints();
    size->left = 0;
    size->top = 0;
    size->right = static_cast<int32>(hints.preferred_width);
    size->bottom = static_cast<int32>(hints.preferred_height);
    return kResultTrue;
}

tresult PLUGIN_API PulpPlugView::onSize(ViewRect* newSize) {
    if (!newSize) return kInvalidArgument;

    auto result = CPluginView::onSize(newSize);

    uint32_t w = static_cast<uint32_t>(newSize->getWidth());
    uint32_t h = static_cast<uint32_t>(newSize->getHeight());
    bridge_.resize(w, h);
    if (editor_host_) {
        editor_host_->set_size(w, h);
    }

    return result;
}

} // namespace pulp::format::vst3

#endif // PULP_VST3_GUI
