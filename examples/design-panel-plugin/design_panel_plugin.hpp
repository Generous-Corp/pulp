#pragma once

// A plugin whose editor IS an imported design.
//
// The shape here is deliberately the one Forge's C++ export emits: the DesignIR
// travels WITH the plugin as an embedded string, and `create_view()` parses it
// and materializes it natively. A takeaway plugin has no importer, no browser
// and no ui.js around it at runtime, so anything the editor needs has to be in
// the binary. This example exists so that path is exercised by a real
// multi-format build rather than only by the importer's own harness.

#include <pulp/format/processor.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
// The complete View type, not a forward declaration: create_view() returns a
// unique_ptr, and every format entry TU instantiates its deleter.
#include <pulp/view/view.hpp>
// Knob / Fader / Meter are used by value in the binding context below.
#include <pulp/view/widgets.hpp>

#include "embedded_design_ir.hpp"
#include "tape_echo_dsp.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>

namespace pulp::examples {

/// IDs are private; the panel binds by NAME. Each control carries its own
/// `pulpParamKey` so the design states which parameter it drives, rather than
/// the two agreeing on an ordering neither can see.
enum DesignPanelParams : state::ParamID {
    kTime = 1,
    kFeedback = 2,
    kTone = 3,
    kMix = 4,
};

/// Resolves a control's `pulpParamKey` against the parameter of the same name.
///
/// Forge's own exporter resolves `param_N` positionally instead. Either
/// contract works, but they are not interchangeable: a panel authored against
/// one and bound by the other silently binds NOTHING — every knob renders,
/// turns, and moves no parameter, which no screenshot and no validator can
/// show. Hence the test that asserts every key in the IR resolves.
class NameBindingContext final : public view::NativeImportBindingContext {
public:
    explicit NameBindingContext(state::StateStore& store,
                                const TapeEcho& echo)
        : store_(store), echo_(echo) {}

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
        // Driven from real output level, so an idle plugin reads zero rather
        // than whatever a parameter happens to say.
        meter_ = &meter;
    }

    /// Non-null once the panel has been bound and it declared a meter.
    view::Meter* meter() const { return meter_; }

    /// Every key the design asked for, resolved or not — the test reads this
    /// rather than inferring success from a plugin that merely opened.
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
        if (key == "time") return kTime;
        if (key == "feedback") return kFeedback;
        if (key == "tone") return kTone;
        if (key == "mix") return kMix;
        return 0;
    }

    state::StateStore& store_;
    const TapeEcho& echo_;
    view::Meter* meter_ = nullptr;
    std::vector<std::pair<std::string, bool>> resolutions_;
};

class DesignPanelProcessor : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpDesignPanel",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.design-panel",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = false,
            .produces_midi = false,
            .tail_samples = 0,
            .supports_f64_audio = true,
        };
    }

    void define_parameters(state::StateStore& store) override {
        // Ranges are the plugin's, not the panel's: a design states WHICH
        // parameter a control drives, never what it means.
        store.add_parameter({
            .id = kTime,
            .name = "Time",
            .unit = "s",
            .range = {0.02f, 2.0f, 0.38f, 0.0f},
        });
        store.add_parameter({
            .id = kFeedback,
            .name = "Feedback",
            .unit = "",
            .range = {0.0f, 0.98f, 0.45f, 0.0f},
        });
        store.add_parameter({
            .id = kTone,
            .name = "Tone",
            .unit = "",
            .range = {0.0f, 1.0f, 0.55f, 0.0f},
        });
        store.add_parameter({
            .id = kMix,
            .name = "Mix",
            .unit = "",
            .range = {0.0f, 1.0f, 0.38f, 0.0f},
        });
    }

    /// The editor's size, taken from the design itself.
    ///
    /// Most imported-design plugins get this from the `PULP_PLUGIN_DESIGN_W/H`
    /// CMake args, but a plugin that CARRIES its IR already knows: the root
    /// states its own width and height, so reading them keeps the window and
    /// the document from disagreeing. Without this the base class falls back to
    /// `editor_size()` — 400x300 — and the panel renders correctly into a
    /// window far too small for it, which looks like a layout bug and is not.
    format::ViewSize view_size() const override {
        const auto& root = embedded_design().root.style;
        const auto w = static_cast<std::uint32_t>(root.width.value_or(0.0f));
        const auto h = static_cast<std::uint32_t>(root.height.value_or(0.0f));
        if (w == 0 || h == 0) return format::Processor::view_size();
        return format::view_size_from_design(w, h, w / 2, h / 2, w * 2, h * 2);
    }

    /// The panel, materialized from the IR the binary carries.
    ///
    /// Returning nullptr when the document is empty is deliberate: an editor
    /// that silently falls back to an auto-generated UI would make a dropped
    /// design look like a design choice, which is the failure this whole
    /// example is meant to make visible.
    std::unique_ptr<view::View> create_view() override {
        const view::DesignIR& ir = embedded_design();
        if (ir.root.children.empty()) return nullptr;
        auto root = view::build_native_view_tree(ir, ir.asset_manifest);
        if (root == nullptr) return root;
        // Building the tree deliberately installs no callbacks; binding is a
        // separate, opt-in step. Skipping it yields a panel that renders and
        // turns and drives nothing — which every validator, screenshot and
        // dlopen check passes.
        binding_ = std::make_unique<NameBindingContext>(state(), echo_);
        view::bind_native_view_tree(*root, ir, *binding_);
        return root;
    }

    /// The binding context the editor was wired with, for the test that asserts
    /// every control resolved. Exposed rather than inferred: "the plugin
    /// opened" is not evidence that anything connected.
    const NameBindingContext* binding_for_test() const { return binding_.get(); }

private:
    TapeEcho echo_;
    /// Channel pointers handed to the DSP. Sized in prepare() so process()
    /// allocates nothing — the one rule the audio thread cannot bend.
    std::array<float*, 32> scratch_{};
    std::unique_ptr<NameBindingContext> binding_;

    /// Parsed once. `view_size()` and `create_view()` must agree about the
    /// document, and re-parsing 100+ KB of JSON per editor open is waste.
    static const view::DesignIR& embedded_design() {
        static const view::DesignIR ir =
            view::parse_design_ir_json(std::string(kEmbeddedDesignIr));
        return ir;
    }

public:

    void prepare(const format::PrepareContext& context) override {
        echo_.prepare(context.sample_rate,
                      std::max(1, context.output_channels));
    }

    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>& audio_input,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const std::size_t channels =
            std::min(audio_output.num_channels(), audio_input.num_channels());
        std::size_t frames = 0;
        for (std::size_t ch = 0; ch < channels; ++ch) {
            auto out = audio_output.channel(ch);
            const auto in = audio_input.channel(ch);
            frames = std::min(out.size(), in.size());
            for (std::size_t i = 0; i < frames; ++i) out[i] = in[i];
            scratch_[ch] = out.data();
        }
        if (channels == 0 || frames == 0) return;

        // Parameters are read once per block, not per sample: the audio thread
        // reads relaxed atomics and the DSP smooths internally, so a block is
        // the right granularity and a per-sample read buys nothing.
        echo_.set_time_seconds(state().get_value(kTime));
        echo_.set_feedback(state().get_value(kFeedback));
        echo_.set_tone(state().get_value(kTone));
        echo_.set_mix(state().get_value(kMix));

        echo_.process(scratch_.data(), static_cast<int>(channels),
                      static_cast<int>(frames));
    }
};

inline std::unique_ptr<format::Processor> create_design_panel() {
    return std::make_unique<DesignPanelProcessor>();
}

}  // namespace pulp::examples
