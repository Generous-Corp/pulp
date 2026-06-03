#include "elysium_ruif_processor.hpp"

#include "elysium_imported_ui.hpp"

#include <pulp/view/view.hpp>

#include <algorithm>

namespace pulp::examples {

format::PluginDescriptor ElysiumRuifProcessor::descriptor() const {
    return {
        .name = "PulpElysiumRuifCppBaseline",
        .manufacturer = "Pulp",
        .bundle_id = "com.pulp.elysium-ruif-cpp-baseline",
        .version = "0.1.0",
        .category = format::PluginCategory::Effect,
        .input_buses = {{"Audio In", 2}},
        .output_buses = {{"Audio Out", 2}},
        .accepts_midi = false,
        .produces_midi = false,
        .tail_samples = 0,
    };
}

void ElysiumRuifProcessor::process(audio::BufferView<float>& output,
                                   const audio::BufferView<const float>& input,
                                   midi::MidiBuffer&,
                                   midi::MidiBuffer&,
                                   const format::ProcessContext&) {
    const auto channels = std::min(output.num_channels(), input.num_channels());
    for (std::size_t ch = 0; ch < channels; ++ch) {
        auto* out = output.channel_ptr(ch);
        const auto* in = input.channel_ptr(ch);
        if (out == nullptr)
            continue;
        if (in == nullptr) {
            std::fill_n(out, output.num_samples(), 0.0f);
            continue;
        }
        std::copy_n(in, output.num_samples(), out);
    }
    for (std::size_t ch = channels; ch < output.num_channels(); ++ch) {
        auto* out = output.channel_ptr(ch);
        if (out != nullptr)
            std::fill_n(out, output.num_samples(), 0.0f);
    }
}

std::unique_ptr<view::View> ElysiumRuifProcessor::create_view() {
    auto root = build_imported_ui();
    if (root)
        root->set_requires_gpu_host(true);
    return root;
}

std::unique_ptr<format::Processor> create_elysium_ruif_processor() {
    return std::make_unique<ElysiumRuifProcessor>();
}

}  // namespace pulp::examples
