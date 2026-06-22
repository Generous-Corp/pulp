#pragma once

// Three.js rotating-cube AUv3 demo.
//
// A near-silent effect plug-in whose editor pane runs `three.webgpu.js`
// via JSC on top of Pulp's Dawn/Metal `GpuSurface`. The audio path is
// deliberately a bit-perfect pass-through — the focus is the editor.
//
// The Processor owns its own `ScriptedUiSession` and exposes it via
// `active_scripted_ui()` so the iOS AU view controller can route the
// host `PluginViewHost::gpu_surface()` into the bridge after
// `PluginViewHost::create` returns.
//
// Success log markers (search syslog/Xcode console):
//   - `[plugin-gpu-host] GpuSurface attached to WidgetBridge via ScriptedUiSession (iOS AUv3)`
//   - `PULP_THREE_DEMO: IIFE loaded (N bytes)`
//   - `PULP_THREE_DEMO: scene script loaded (N bytes)`
//   - `PULP_THREEJS: globalThis.THREE available (NN exports)`
//   - `PULP_THREE_SHIM: ready`
//   - `PULP_THREE_SHIM: webgpu-renderer-present`
//   - `PULP_THREE_RENDER: first frame submitted`
//
// Failure modes are deliberately loud + visible-on-device:
//   - `pulp::view::threejs_iife_source()` returns nullopt → log
//     `PULP_THREE_DEMO: three.iife.js missing from bundle — Three.js not loaded`
//     and the editor paints a "Three.js not loaded" placeholder so the
//     failure is obvious without a console attach.
//   - scene.js missing → similar placeholder.
//
// Use this example to verify that the AUv3 editor can load the bundled
// Three.js IIFE, attach Pulp's presentable GPU surface, and submit a
// WebGPU frame on device.

#include <pulp/format/processor.hpp>
#include <pulp/view/scripted_ui.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace pulp::examples::ios_threejs {

class PulpThreeJsDemo : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override;
    void define_parameters(pulp::state::StateStore& store) override;
    void prepare(const pulp::format::PrepareContext& ctx) override;
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& ctx) override;

    // The HostApp container opens the editor at 540x720 on a Pro 11"
    // 3rd-gen by default. The scripted UI lays everything out in
    // logical pixels relative to this size; the iOS AU view controller
    // calls `set_size()` on every layout pass so resize-on-rotate
    // works the same way the Chainer demo does.
    std::pair<uint32_t, uint32_t> editor_size() const override {
        return {540, 720};
    }

    std::unique_ptr<pulp::view::View> create_view() override;

    pulp::view::ScriptedUiSession* active_scripted_ui() override {
        return scripted_ui_.get();
    }
    const pulp::view::ScriptedUiSession* active_scripted_ui() const override {
        return scripted_ui_.get();
    }

    void on_view_closed(pulp::view::View& view) override;

private:
    // The bundled-script path is resolved at create_view() time from
    // the .appex via NSBundle. We write a concatenated
    // `IIFE + shim + scene` script to NSTemporaryDirectory and hand
    // THAT to the ScriptedUiSession so the bridge loads everything in
    // a single `WidgetBridge::load_script` call. (The bridge has no
    // pre-load native-registration hook today — concatenation is the
    // smallest change that gets `globalThis.THREE` populated before
    // user scene code runs.)
    std::filesystem::path concatenated_script_path_;

    std::unique_ptr<pulp::view::ScriptedUiSession> scripted_ui_;
};

}  // namespace pulp::examples::ios_threejs
