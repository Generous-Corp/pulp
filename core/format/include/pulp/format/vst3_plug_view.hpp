#pragma once

// VST3 IPlugView implementation for Pulp
// Creates an AutoUi-based editor view that embeds in the host's window.
// Uses PluginViewHost for platform-native rendering (CoreGraphics or GPU).
//
// Built from VST3 SDK (MIT license) CPluginView base class.

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#ifdef PULP_VST3_GUI

#include <pulp/view/auto_ui.hpp>
#include <pulp/view/plugin_view_host.hpp>

// VST3 SDK
#include <public.sdk/source/common/pluginview.h>

namespace pulp::format::vst3 {

// IPlugView implementation that hosts an AutoUi-generated view tree
// inside the DAW's editor window via PluginViewHost
class PulpPlugView : public Steinberg::CPluginView {
public:
    PulpPlugView(Processor& processor, state::StateStore& store);

    // IPlugView overrides
    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;

private:
    Processor& processor_;
    state::StateStore& store_;
    std::unique_ptr<view::View> editor_root_;
    std::unique_ptr<view::PluginViewHost> editor_host_;
};

} // namespace pulp::format::vst3

#endif // PULP_VST3_GUI
