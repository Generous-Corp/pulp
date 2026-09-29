#pragma once

// A minimal effect whose "Freeze" toggle switches the reported tail between
// none and infinite, flagging the change from the audio thread on each edge.
// A host that automates the toggle therefore drives the adapter's deferred
// restart path (VST3 kReloadComponent) through activation transitions.

#include <pulp/format/processor.hpp>

#include <atomic>
#include <cstddef>
#include <memory>

namespace pulp::test_fixtures {

enum TailEdgeProbeParams : state::ParamID {
    kFreeze = 1,
};

class TailEdgeProbeProcessor final : public format::Processor {
public:
    format::PluginDescriptor descriptor() const override {
        return {
            .name = "Pulp Test (dev) Plugin",
            .manufacturer = "Pulp",
            .bundle_id = "dev.pulp.test.bundle-name-quoting",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
            .accepts_midi = false,
            .produces_midi = false,
            .tail_samples = tail_samples_.load(std::memory_order_acquire),
        };
    }

    void define_parameters(state::StateStore& store) override {
        store.add_parameter({
            .id = kFreeze,
            .name = "Freeze",
            .unit = "",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
            .kind = state::ParamKind::Toggle,
        });
    }

    void prepare(const format::PrepareContext&) override {}

    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 midi::MidiBuffer&,
                 midi::MidiBuffer&,
                 const format::ProcessContext&) override {
        const bool freeze = state().get_value(kFreeze) >= 0.5f;
        if (freeze != frozen_) {
            frozen_ = freeze;
            tail_samples_.store(freeze ? -1 : 0, std::memory_order_release);
            flag_tail_changed();
        }
        for (std::size_t ch = 0; ch < output.num_channels(); ++ch) {
            auto out = output.channel(ch);
            if (ch < input.num_channels()) {
                auto in = input.channel(ch);
                for (std::size_t i = 0; i < output.num_samples(); ++i) out[i] = in[i];
            } else {
                for (std::size_t i = 0; i < output.num_samples(); ++i) out[i] = 0.0f;
            }
        }
    }

private:
    bool frozen_ = false;
    std::atomic<int> tail_samples_{0};
};

inline std::unique_ptr<format::Processor> create_tail_edge_probe() {
    return std::make_unique<TailEdgeProbeProcessor>();
}

}  // namespace pulp::test_fixtures
