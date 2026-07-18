#pragma once

// VST3 IPlugView implementation for Pulp
// Creates an AutoUi-based editor view that embeds in the host's window.
// Uses PluginViewHost for platform-native rendering (CoreGraphics or GPU).
//
// Built from VST3 SDK (MIT license) CPluginView base class.

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>

#ifdef PULP_VST3_GUI

#include <pulp/format/view_bridge.hpp>
#include <pulp/view/plugin_view_host.hpp>

// VST3 SDK
#include <public.sdk/source/common/pluginview.h>

namespace pulp::format::vst3 {

// IPlugView implementation that owns a ViewBridge. The bridge builds the
// view tree (custom create_view() or scripted/AutoUi fallback), dispatches
// lifecycle callbacks on the Processor, and tracks secondary views.
class PulpPlugView : public Steinberg::CPluginView {
public:
    PulpPlugView(Processor& processor, state::StateStore& store,
                 runtime::AliveToken::Handle owner_alive = {});

    // IPlugView overrides
    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;
    // Resize negotiation (proportional + aspect-locked). The host calls
    // checkSizeConstraint() during a user drag to snap the candidate
    // rect to the editor's design aspect; onSize() then applies it. The
    // PluginViewHost scales content to fit via its design viewport.
    Steinberg::tresult PLUGIN_API canResize() override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect) override;

private:
    friend struct PulpPlugViewTestAccess;
    Processor& processor_;
    state::StateStore& store_;
    ViewBridge bridge_;
    std::unique_ptr<view::PluginViewHost> editor_host_;
};

} // namespace pulp::format::vst3

#endif // PULP_VST3_GUI
