#include "elysium_ruif_processor.hpp"
#include "elysium_rust_provider_bridge.hpp"

#include <pulp/runtime/log.hpp>

#include <algorithm>

namespace pulp::examples {
namespace {

void copy_audio_passthrough(audio::BufferView<float>& output,
                            const audio::BufferView<const float>& input) {
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

class ElysiumRuifRustProviderProcessor final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "PulpElysiumRuifRustProvider",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.elysium-ruif-rust-provider",
            .version = "0.1.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = false,
            .produces_midi = false,
            .tail_samples = 0,
        };
    }

    void define_parameters(state::StateStore&) override {}
    void prepare(const format::PrepareContext&) override {}
    void release() override {}

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        copy_audio_passthrough(output, input);
    }

    std::pair<uint32_t, uint32_t> editor_size() const override { return {1000, 600}; }

    std::unique_ptr<view::View> create_view() override {
        std::string failure_reason;
        auto root = build_elysium_rust_provider_ui(&failure_reason);
        if (!root && !failure_reason.empty())
            runtime::log_error("ELYSIUM Rust-provider editor failed: {}", failure_reason);
        return root;
    }
};

}  // namespace

std::unique_ptr<format::Processor> create_elysium_ruif_rust_provider_processor() {
    return std::make_unique<ElysiumRuifRustProviderProcessor>();
}

}  // namespace pulp::examples
