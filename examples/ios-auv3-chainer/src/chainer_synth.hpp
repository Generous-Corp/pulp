// Phase iOS-D.2 — Chainer-style iOS AUv3 example.
//
// Smallest viable JS-UI-style synth on iPad GPU: a sine instrument
// driven by ONE parameter (Drive, exposed on the AU parameter tree)
// whose editor view paints a Pulp-native widget tree (title Label +
// Knob bound to Drive + Meter showing audio output) through the same
// Skia/Dawn GPU path the iOS-D.1 smoke proved end-to-end.
//
// On iOS, JS-driven editor wiring (script_engine + widget_bridge) is
// not yet validated in-process inside an AUv3 .appex — see
// planning/2026-05-24-auv3-ios-validation.md Phase iOS-D. This example
// uses the native widget tree as the equivalent "JS UI shape" so the
// visible deliverable lands without dragging an iOS QuickJS bring-up
// onto the critical path.

#pragma once

#include <pulp/format/processor.hpp>

#include <atomic>
#include <memory>

namespace pulp::examples::ios_chainer {

class ChainerSynth : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override;
    void define_parameters(pulp::state::StateStore& store) override;
    void prepare(const pulp::format::PrepareContext& ctx) override;
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::format::ProcessContext& ctx) override;

    std::unique_ptr<pulp::view::View> create_view() override;

    // The editor pulls the latest output peak through this hook so the
    // Meter widget on the GPU path reflects real render-block activity
    // without coupling the View to the audio buffer directly.
    float consume_peak() {
        return peak_.exchange(0.0f, std::memory_order_acq_rel);
    }

    pulp::state::ParamID drive_param_id() const { return drive_param_id_; }

private:
    pulp::state::ParamID drive_param_id_ = 1;
    double sample_rate_ = 48000.0;
    double phase_ = 0.0;
    float gate_ = 0.0f;
    float gate_target_ = 0.0f;
    // Audio thread writes / UI thread reads. atomic<float> works because
    // we only publish the latest peak, never a coherent multi-field
    // snapshot. release on store + acquire on exchange-load.
    std::atomic<float> peak_{0.0f};
};

} // namespace pulp::examples::ios_chainer
