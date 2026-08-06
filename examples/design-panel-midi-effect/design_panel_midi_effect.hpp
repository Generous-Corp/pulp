#pragma once

// A MIDI EFFECT whose editor is an imported design.
//
// Third of the three plugin kinds, and the one least like the other two. It
// has no audio path at all: notes in, notes out. On Apple that is component
// type `aumi` on a `MusicDeviceBase`, not `AUEffectBase`, because a MIDI
// processor has no audio input to pull.
//
// The panel path is identical to the effect and instrument examples, which is
// the point — a design panel should not care what the plugin underneath it
// does. This example is what proves that claim rather than assuming it.

#include <pulp/format/processor.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include "embedded_design_ir.hpp"
#include "lattice_arp_dsp.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace pulp::examples {

enum MidiEffectParams : state::ParamID {
    kRate = 1,
    kGate = 2,
    kSwing = 3,
    kOctaves = 4,
    kVelocity = 5,
};

/// Resolves a control's `pulpParamKey` against the parameter of the same name.
class MidiEffectBindingContext final : public view::NativeImportBindingContext {
public:
    explicit MidiEffectBindingContext(state::StateStore& store)
        : store_(store) {}

    void bind_knob(view::Knob& knob,
                   const view::NativeImportBindingDescriptor& d) override {
        attach(knob, d.param_key);
    }
    void bind_fader(view::Fader& fader,
                    const view::NativeImportBindingDescriptor& d) override {
        attach(fader, d.param_key);
    }
    void bind_meter(view::Meter& meter,
                    const view::NativeImportMeterBindingDescriptor&) override {
        meter_ = &meter;
    }

    view::Meter* meter() const { return meter_; }

    const std::vector<std::pair<std::string, bool>>& resolutions() const {
        return resolutions_;
    }

private:
    template <typename Control>
    void attach(Control& control, std::string_view key) {
        const auto id = id_for(key);
        resolutions_.emplace_back(std::string(key), id != 0);
        if (id == 0) return;
        control.set_value(store_.get_value(id));
        control.on_change = [this, id](float v) { store_.set_value(id, v); };
    }

    static state::ParamID id_for(std::string_view key) {
        if (key == "rate") return kRate;
        if (key == "gate") return kGate;
        if (key == "swing") return kSwing;
        if (key == "octaves") return kOctaves;
        if (key == "velocity") return kVelocity;
        return 0;
    }

    state::StateStore& store_;
    view::Meter* meter_ = nullptr;
    std::vector<std::pair<std::string, bool>> resolutions_;
};

class DesignPanelMidiEffect : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpDesignArp",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.design-arp",
            .version = "1.0.0",
            .category = format::PluginCategory::MidiEffect,
            // No audio buses in either direction. Declaring an output bus here
            // makes hosts offer it as an instrument and then wonder why it is
            // silent.
            .input_buses = {},
            .output_buses = {},
            .accepts_midi = true,
            .produces_midi = true,
            .tail_samples = 0,
            .supports_f64_audio = false,
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kRate, .name = "Rate", .unit = "Hz",
            .range = {0.5f, 20.0f, 8.0f, 0.0f},
        });
        store.add_parameter({
            .id = kGate, .name = "Gate", .unit = "",
            .range = {0.05f, 1.0f, 0.5f, 0.0f},
        });
        store.add_parameter({
            .id = kSwing, .name = "Swing", .unit = "",
            .range = {0.0f, 0.75f, 0.0f, 0.0f},
        });
        store.add_parameter({
            .id = kOctaves, .name = "Octaves", .unit = "",
            .range = {1.0f, 4.0f, 1.0f, 0.0f},
        });
        store.add_parameter({
            .id = kVelocity, .name = "Velocity", .unit = "",
            .range = {0.0f, 1.0f, 0.8f, 0.0f},
        });
    }

    format::ViewSize view_size() const override {
        const auto& root = embedded_design().root.style;
        const auto w = static_cast<std::uint32_t>(root.width.value_or(0.0f));
        const auto h = static_cast<std::uint32_t>(root.height.value_or(0.0f));
        if (w == 0 || h == 0) return format::Processor::view_size();
        return format::view_size_from_design(w, h, w / 2, h / 2, w * 2, h * 2);
    }

    std::unique_ptr<view::View> create_view() override {
        const view::DesignIR& ir = embedded_design();
        if (ir.root.children.empty()) return nullptr;
        auto root = view::build_native_view_tree(ir, ir.asset_manifest);
        if (root == nullptr) return root;
        binding_ = std::make_unique<MidiEffectBindingContext>(state());
        view::bind_native_view_tree(*root, ir, *binding_);
        return root;
    }

    const MidiEffectBindingContext* binding_for_test() const {
        return binding_.get();
    }

    void prepare(const format::PrepareContext& context) override {
        arp_.prepare(context.sample_rate);
    }

    void process(audio::BufferView<float>&,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_input, midi::MidiBuffer& midi_output,
                 const format::ProcessContext& context) override {
        arp_.set_rate_hz(state().get_value(kRate));
        arp_.set_gate(state().get_value(kGate));
        arp_.set_swing(state().get_value(kSwing));
        arp_.set_octaves(static_cast<int>(std::lround(state().get_value(kOctaves))));
        arp_.set_velocity(state().get_value(kVelocity));

        const int frames = context.num_samples;

        // Incoming notes are consumed, not forwarded. An arpeggiator that
        // passes its input through plays the held chord underneath the
        // pattern, which is a different instrument.
        midi_input.sort();
        std::size_t event = 0;
        int rendered = 0;
        while (rendered < frames) {
            while (event < midi_input.size() &&
                   midi_input[event].sample_offset <= rendered) {
                const auto& e = midi_input[event];
                if (e.is_note_on() && e.velocity() > 0) arp_.note_on(e.note());
                else if (e.is_note_off() || e.is_note_on()) arp_.note_off(e.note());
                ++event;
            }
            int next = frames;
            if (event < midi_input.size())
                next = std::min(frames,
                                std::max(rendered + 1,
                                         static_cast<int>(
                                             midi_input[event].sample_offset)));
            arp_.process(midi_output, next - rendered);
            rendered = next;
        }

        // Nothing held and nothing left sounding: release so the instrument
        // downstream is not left holding a note the player has let go of.
        if (arp_.held() == 0)
            arp_.release_all(midi_output, std::max(0, frames - 1));
    }

    const LatticeArp& arp_for_test() const { return arp_; }

private:
    LatticeArp arp_;
    std::unique_ptr<MidiEffectBindingContext> binding_;

    static const view::DesignIR& embedded_design() {
        static const view::DesignIR ir =
            view::parse_design_ir_json(std::string(kEmbeddedDesignIr));
        return ir;
    }
};

inline std::unique_ptr<format::Processor> create_design_panel_midi_effect() {
    return std::make_unique<DesignPanelMidiEffect>();
}

}  // namespace pulp::examples
