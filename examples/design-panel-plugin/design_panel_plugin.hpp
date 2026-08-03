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

#include "embedded_design_ir.hpp"

#include <algorithm>
#include <memory>
#include <string>

namespace pulp::examples {

enum DesignPanelParams : state::ParamID {
    kMacroA = 1,
    kMacroB = 2,
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
        store.add_parameter({
            .id = kMacroA,
            .name = "Macro A",
            .unit = "",
            .range = {0.0f, 1.0f, 0.5f, 0.0f},
        });
        store.add_parameter({
            .id = kMacroB,
            .name = "Macro B",
            .unit = "",
            .range = {0.0f, 1.0f, 0.5f, 0.0f},
        });
    }

    /// The panel, materialized from the IR the binary carries.
    ///
    /// Returning nullptr when the document is empty is deliberate: an editor
    /// that silently falls back to an auto-generated UI would make a dropped
    /// design look like a design choice, which is the failure this whole
    /// example is meant to make visible.
    std::unique_ptr<view::View> create_view() override {
        view::DesignIR ir =
            view::parse_design_ir_json(std::string(kEmbeddedDesignIr));
        if (ir.root.children.empty()) return nullptr;
        return view::build_native_view_tree(ir, ir.asset_manifest);
    }

    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>& audio_output,
                 const audio::BufferView<const float>& audio_input,
                 midi::MidiBuffer&, midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        // Deliberately a pass-through: this example is about what the editor
        // draws, and a gain stage here would only add a way for it to fail.
        const std::size_t channels =
            std::min(audio_output.num_channels(), audio_input.num_channels());
        for (std::size_t ch = 0; ch < channels; ++ch) {
            auto out = audio_output.channel(ch);
            const auto in = audio_input.channel(ch);
            const std::size_t frames = std::min(out.size(), in.size());
            for (std::size_t i = 0; i < frames; ++i) out[i] = in[i];
        }
    }
};

inline std::unique_ptr<format::Processor> create_design_panel() {
    return std::make_unique<DesignPanelProcessor>();
}

}  // namespace pulp::examples
