#pragma once

// An INSTRUMENT whose editor IS an imported design.
//
// The sibling `design-panel-plugin` proves the same path for an audio effect.
// This one exists because an instrument is not an effect with the buses
// relabelled: it declares no audio input, it is driven entirely by MIDI, and
// on Apple it registers as component type `aumu` rather than `aufx`. A design
// panel that works on an effect tells you nothing about whether the same panel
// survives an instrument's descriptor, and hosts scan the two differently.

#include <pulp/format/processor.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include "embedded_design_ir.hpp"
#include "kelvin_synth_dsp.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace pulp::examples {

/// IDs are private; the panel binds by NAME, each control carrying its own
/// `pulpParamKey`. See the effect example for why this contract is asserted
/// rather than assumed: a panel bound by a different convention binds NOTHING,
/// and renders identically while doing it.
enum InstrumentParams : state::ParamID {
    kAttack = 1,
    kRelease = 2,
    kCutoff = 3,
    kResonance = 4,
    kDrive = 5,
};

/// Resolves a control's `pulpParamKey` against the parameter of the same name.
class InstrumentBindingContext final : public view::NativeImportBindingContext {
public:
    explicit InstrumentBindingContext(state::StateStore& store)
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
        // Driven from real output level, so an instrument with no notes held
        // reads zero rather than whatever a parameter happens to say.
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
        if (key == "attack") return kAttack;
        if (key == "release") return kRelease;
        if (key == "cutoff") return kCutoff;
        if (key == "resonance") return kResonance;
        if (key == "drive") return kDrive;
        return 0;
    }

    state::StateStore& store_;
    view::Meter* meter_ = nullptr;
    std::vector<std::pair<std::string, bool>> resolutions_;
};

class DesignPanelInstrument : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpDesignSynth",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.design-synth",
            .version = "1.0.0",
            .category = format::PluginCategory::Instrument,
            // No audio input bus. An instrument that declares one is offered
            // sidechain slots it cannot use, and some hosts will refuse to
            // instantiate it on an instrument track at all.
            .input_buses = {},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = true,
            .produces_midi = false,
            // -1 is infinite, which is what an instrument means: it can sound
            // whenever a note arrives, and its release runs for up to eight
            // seconds after the last note-off. Claiming 0 lets a host stop
            // pulling at transport stop and truncates every release into a
            // click, which sounds like a DSP bug and is a descriptor bug.
            .tail_samples = -1,
            .supports_f64_audio = false,
        };
    }

    void define_parameters(state::StateStore& store) override {
        // Ranges are the plugin's, not the panel's: a design states WHICH
        // parameter a control drives, never what it means.
        store.add_parameter({
            .id = kAttack, .name = "Attack", .unit = "s",
            .range = {0.001f, 4.0f, 0.01f, 0.0f},
        });
        store.add_parameter({
            .id = kRelease, .name = "Release", .unit = "s",
            .range = {0.005f, 8.0f, 0.30f, 0.0f},
        });
        store.add_parameter({
            .id = kCutoff, .name = "Cutoff", .unit = "",
            .range = {0.0f, 1.0f, 0.62f, 0.0f},
        });
        store.add_parameter({
            .id = kResonance, .name = "Resonance", .unit = "",
            .range = {0.0f, 1.0f, 0.22f, 0.0f},
        });
        store.add_parameter({
            .id = kDrive, .name = "Drive", .unit = "",
            .range = {0.0f, 1.0f, 0.15f, 0.0f},
        });
    }

    /// The editor's size, taken from the design itself rather than from the
    /// 400x300 base-class default, which would render the panel correctly into
    /// a window far too small for it.
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
        // Building installs no callbacks; binding is a separate, opt-in step.
        // Skipping it yields a panel that renders and turns and drives nothing.
        binding_ = std::make_unique<InstrumentBindingContext>(state());
        view::bind_native_view_tree(*root, ir, *binding_);
        return root;
    }

    const InstrumentBindingContext* binding_for_test() const {
        return binding_.get();
    }

    void prepare(const format::PrepareContext& context) override {
        synth_.prepare(context.sample_rate, std::max(1, context.max_buffer_size));
    }

    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>&,
                 midi::MidiBuffer& midi_input, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const std::size_t channels = audio_output.num_channels();
        if (channels == 0) return;
        auto left = audio_output.channel(0);
        const int frames = static_cast<int>(left.size());
        if (frames == 0) return;

        // An instrument owns its output buffer rather than adding to whatever
        // the host left there. Not clearing produces the classic symptom of
        // "the synth is noisy when idle".
        std::fill(left.begin(), left.end(), 0.0f);

        // Parameters are read once per block: the audio thread reads relaxed
        // atomics and the DSP smooths internally, so a block is the right
        // granularity.
        synth_.set_attack(state().get_value(kAttack));
        synth_.set_release(state().get_value(kRelease));
        synth_.set_cutoff(state().get_value(kCutoff));
        synth_.set_resonance(state().get_value(kResonance));
        synth_.set_drive(state().get_value(kDrive));

        // Events are dispatched at their sample offsets rather than all at the
        // top of the block, or every note in a chord quantises to the block
        // boundary and timing collapses at large buffer sizes.
        midi_input.sort();
        std::size_t event = 0;
        int rendered = 0;
        while (rendered < frames) {
            while (event < midi_input.size() &&
                   midi_input[event].sample_offset <= rendered) {
                const auto& e = midi_input[event];
                if (e.is_note_on() && e.velocity() > 0)
                    synth_.note_on(e.note(), e.velocity() / 127.0f);
                else if (e.is_note_off() || e.is_note_on())
                    synth_.note_off(e.note());
                ++event;
            }
            int next = frames;
            if (event < midi_input.size())
                next = std::min(frames,
                                std::max(rendered + 1,
                                         static_cast<int>(
                                             midi_input[event].sample_offset)));
            synth_.render(left.data() + rendered, next - rendered);
            rendered = next;
        }

        // Mono voice engine fanned to every output channel, so a stereo track
        // is centred rather than silent on the right.
        for (std::size_t ch = 1; ch < channels; ++ch) {
            auto dst = audio_output.channel(ch);
            std::copy(left.begin(), left.begin() +
                      static_cast<std::ptrdiff_t>(
                          std::min(dst.size(), left.size())), dst.begin());
        }
    }

    /// Exposed for the tests, which assert on what was produced rather than
    /// inferring from a plugin that merely instantiated.
    const KelvinSynth& synth_for_test() const { return synth_; }

private:
    KelvinSynth synth_;
    std::unique_ptr<InstrumentBindingContext> binding_;

    /// Parsed once. `view_size()` and `create_view()` must agree about the
    /// document, and re-parsing 270+ KB of JSON per editor open is waste.
    static const view::DesignIR& embedded_design() {
        static const view::DesignIR ir =
            view::parse_design_ir_json(std::string(kEmbeddedDesignIr));
        return ir;
    }
};

inline std::unique_ptr<format::Processor> create_design_panel_instrument() {
    return std::make_unique<DesignPanelInstrument>();
}

}  // namespace pulp::examples
