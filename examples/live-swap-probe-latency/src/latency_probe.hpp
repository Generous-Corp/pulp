#pragma once

// P0.5 PROBE (Phase 0 derisk — NOT for main). A minimal effect whose reported
// latency flips between 0 and kHigh samples based on the "ExtraLatency" param,
// calling flag_latency_changed() when it changes so the format adapter renotifies
// the host. Loaded in REAPER to observe whether the host renegotiates PDC on a
// runtime latency change. Passthrough audio: this probes the HOST's reaction to
// the notification, not delay correctness (Phase 2 handles the atomic
// latency/tail snapshot where audio actually shifts).
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>

namespace probe {

class LatencyProbe final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {.name = "Pulp Latency Probe",
                .manufacturer = "Pulp",
                .bundle_id = "com.pulp.examples.latency-probe",
                .version = "0.1.0",
                .category = pulp::format::PluginCategory::Effect,
                .input_buses = {{"In", 2}},
                .output_buses = {{"Out", 2}}};
    }

    void define_parameters(pulp::state::StateStore& s) override {
        // 0 -> 0 samples latency; 1 -> kHigh samples. Host shows it as a slider.
        s.add_parameter({.id = 1, .name = "ExtraLatency", .unit = "",
                         .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    int latency_samples() const override {
        return reported_latency_.load(std::memory_order_relaxed);
    }

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        const int target = (state().get_value(1) >= 0.5f) ? kHigh : 0;
        if (target != reported_latency_.load(std::memory_order_relaxed)) {
            reported_latency_.store(target, std::memory_order_relaxed);
            flag_latency_changed();  // adapter renotifies host next block
        }
        const std::size_t ch = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c);
            auto i = in.channel(c);
            for (std::size_t n = 0; n < out.num_samples(); ++n) o[n] = i[n];
        }
    }

private:
    static constexpr int kHigh = 2048;
    std::atomic<int> reported_latency_{0};
};

inline std::unique_ptr<pulp::format::Processor> create_latency_probe() {
    return std::make_unique<LatencyProbe>();
}

}  // namespace probe
